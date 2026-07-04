// POSIX poll()/select() over Motor's VDSO readiness registry (an epoll/mio-
// shaped API: registry fd + (token, interests) registrations + edge-ish event
// delivery with initial readiness synthesized at registration time).
// Design + verified VDSO semantics: docs/porting-libc-appendix-h.md (H.4).
//
// Strategy: one throwaway registry per call. Correct because every pollable
// source reports its *current* readiness when added; cheap enough for the
// poll-loop programs this targets (one fd alloc + registration round trip).

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/motor-socket.hpp>
#include <mlibc/motor-util.hpp>
#include <moto_rt.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>

namespace {

using mlibc::moto_to_errno;
using mlibc::motor_sock_pollfd;

// Map accumulated Motor event bits to revents, masked by what the caller
// asked for (POLLERR/POLLHUP are always reportable, per POSIX).
short map_events(uint64_t m, short requested) {
	short rev = 0;
	if (m & MOTO_POLL_READABLE)
		rev |= POLLIN | POLLRDNORM;
	if (m & MOTO_POLL_WRITABLE)
		rev |= POLLOUT | POLLWRNORM;
	if (m & MOTO_POLL_READ_CLOSED) // peer half-close: EOF is readable
		rev |= POLLIN | POLLRDNORM | POLLRDHUP;
	if (m & MOTO_POLL_ERROR)
		rev |= POLLERR;
	rev &= (short)(requested | POLLERR | POLLHUP);
	if ((m & MOTO_POLL_READ_CLOSED) && (m & MOTO_POLL_WRITE_CLOSED))
		rev |= POLLHUP; // both directions gone
	return rev;
}

// Sleep for `nanos` (UINT64_MAX = forever), chunked so we never hand the
// shim a duration that could overflow Instant arithmetic.
void plain_sleep(uint64_t nanos) {
	constexpr uint64_t kChunk = 3600ull * 1000000000ull;
	while (nanos) {
		uint64_t chunk = nanos < kChunk ? nanos : kChunk;
		moto_rt_sleep_nanos(chunk);
		if (nanos != UINT64_MAX)
			nanos -= chunk;
	}
}

// The shared core. timeout_ns: UINT64_MAX = infinite, 0 = non-blocking.
// Returns 0 + *num_events (poll() semantics: count of fds with revents != 0)
// or a positive errno.
int do_poll(struct pollfd *fds, nfds_t count, uint64_t timeout_ns, int *num_events) {
	if (count > 4096)
		return EINVAL;

	// Per-entry: resolved real fd (or -1), and the index owning the actual
	// registration (owner[i] == i for registrants; duplicates alias — the
	// registry rejects registering one fd twice, so interests are merged).
	int real_stack[64];
	int owner_stack[64];
	uint64_t ints_stack[64];
	moto_poll_event_t ev_stack[64];
	int *real = real_stack;
	int *owner = owner_stack;
	uint64_t *reg_ints = ints_stack;
	moto_poll_event_t *evbuf = ev_stack;
	void *heap = nullptr;
	if (count > 64) {
		heap = malloc(count * (2 * sizeof(int) + sizeof(uint64_t)
		                       + sizeof(moto_poll_event_t)));
		if (!heap)
			return ENOMEM;
		real = reinterpret_cast<int *>(heap);
		owner = real + count;
		reg_ints = reinterpret_cast<uint64_t *>(owner + count);
		evbuf = reinterpret_cast<moto_poll_event_t *>(reg_ints + count);
	}

	int regfd = -1;
	int err = 0;
	int results = 0;
	size_t nreg = 0;

	// Create the registry BEFORE touching caller fds: fd numbers are reused
	// immediately, so a lazily-created registry can be assigned the number of
	// an fd the caller closed — and a stale entry naming that number would
	// then alias the registry itself instead of reporting POLLNVAL.
	if (count > 0) {
		regfd = moto_rt_poll_new();
		if (regfd < 0) {
			err = moto_to_errno(regfd);
			goto out;
		}
	}

	for (nfds_t i = 0; i < count; i++) {
		fds[i].revents = 0;
		real[i] = -1;
		owner[i] = -1;
		if (fds[i].fd < 0)
			continue; // POSIX: negative fd = ignore the entry

		int r = motor_sock_pollfd(fds[i].fd);
		if (r == -EBADF) {
			fds[i].revents = POLLNVAL;
			continue;
		}
		if (r == -EAGAIN)
			continue; // fresh TCP pseudo-socket: nothing to poll, never ready
		if (r < 0) {
			fds[i].revents = POLLERR; // listener arming failed
			continue;
		}
		if (r == regfd) {
			// The caller can't legitimately hold our just-created registry's
			// fd: this is a closed fd whose number the registry inherited.
			fds[i].revents = POLLNVAL;
			continue;
		}
		real[i] = r;

		uint64_t ints = 0;
		if (fds[i].events & (POLLIN | POLLRDNORM))
			ints |= MOTO_POLL_READABLE;
		if (fds[i].events & (POLLOUT | POLLWRNORM))
			ints |= MOTO_POLL_WRITABLE;
		// ints == 0 is a valid registration: closed/error events are always
		// delivered regardless of interests.

		// Duplicate fd in the array? Merge into the existing registration.
		int dup = -1;
		for (nfds_t j = 0; j < i; j++) {
			if (owner[j] == (int)j && real[j] == r) {
				dup = (int)j;
				break;
			}
		}
		if (dup >= 0) {
			uint64_t merged = reg_ints[dup] | ints;
			if (merged != reg_ints[dup]) {
				int32_t e = moto_rt_poll_set(regfd, r, (uint64_t)dup, merged);
				if (e < 0) {
					err = moto_to_errno(e);
					goto out;
				}
				reg_ints[dup] = merged;
			}
			owner[i] = dup;
			continue;
		}

		{
			int32_t e = moto_rt_poll_add(regfd, r, (uint64_t)i, ints);
			if (e == -MOTO_E_INVALID_ARGUMENT) {
				// Not a pollable kind (regular file/dir): always ready for
				// in/out, per POSIX.
				fds[i].revents = fds[i].events
				                 & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM);
				real[i] = -1;
				continue;
			}
			if (e == -MOTO_E_BAD_HANDLE) {
				fds[i].revents = POLLNVAL;
				real[i] = -1;
				continue;
			}
			if (e < 0) {
				err = moto_to_errno(e);
				goto out;
			}
		}
		owner[i] = (int)i;
		reg_ints[i] = ints;
		nreg++;
	}

	for (nfds_t i = 0; i < count; i++)
		if (fds[i].revents)
			results++;

	if (nreg == 0) {
		// Nothing registered (only files / invalid / never-ready entries, or
		// an empty set). poll() on such a set blocks for the full timeout
		// when nothing is ready.
		if (regfd >= 0) {
			moto_rt_close(regfd);
			regfd = -1;
		}
		if (results == 0 && timeout_ns != 0)
			plain_sleep(timeout_ns);
		goto out;
	}

	{
		uint64_t start = moto_rt_mono_nanos();
		for (;;) {
			uint64_t timo;
			if (results > 0) {
				timo = 0; // answers in hand: harvest current readiness only
			} else if (timeout_ns == UINT64_MAX) {
				timo = UINT64_MAX;
			} else {
				uint64_t elapsed = moto_rt_mono_nanos() - start;
				timo = elapsed >= timeout_ns ? 0 : timeout_ns - elapsed;
			}

			int32_t n = moto_rt_poll_wait(regfd, timo, evbuf, nreg ? nreg : 1);
			if (n < 0) {
				err = moto_to_errno(n);
				break;
			}
			for (int32_t k = 0; k < n; k++) {
				if (evbuf[k].token >= count)
					continue;
				int own = (int)evbuf[k].token;
				for (nfds_t j = 0; j < count; j++) {
					if (owner[j] == own)
						fds[j].revents |= map_events(evbuf[k].events, fds[j].events);
				}
			}
			results = 0;
			for (nfds_t j = 0; j < count; j++)
				if (fds[j].revents)
					results++;
			// timo == 0: this was the final (or non-blocking) harvest.
			// n == 0: timed out. Otherwise, events that mapped to nothing
			// the caller asked for -> keep waiting on the remaining budget.
			if (results > 0 || timo == 0 || n == 0)
				break;
		}
	}

out:
	if (regfd >= 0) {
		for (nfds_t i = 0; i < count; i++)
			if (owner[i] == (int)i)
				moto_rt_poll_del(regfd, real[i]);
		moto_rt_close(regfd);
	}
	if (heap)
		free(heap);
	if (err)
		return err;
	*num_events = results;
	return 0;
}

} // namespace

namespace mlibc {

int Sysdeps<Poll>::operator()(struct pollfd *fds, nfds_t count, int timeout,
                              int *num_events) {
	uint64_t ns = timeout < 0 ? UINT64_MAX : (uint64_t)timeout * 1000000ull;
	return do_poll(fds, count, ns, num_events);
}

int Sysdeps<Pselect>::operator()(int num_fds, fd_set *read_set, fd_set *write_set,
                                 fd_set *except_set, const struct timespec *timeout,
                                 const sigset_t *sigmask, int *num_events) {
	(void)sigmask; // no async signals on Motor; the mask is bookkeeping only
	if (num_fds < 0 || num_fds > FD_SETSIZE)
		return EINVAL;
	if (timeout
	    && (timeout->tv_sec < 0 || timeout->tv_nsec < 0
	        || timeout->tv_nsec >= 1000000000l))
		return EINVAL;

	// Snapshot the input sets; the same sets are also the output.
	fd_set in_read, in_write, in_except;
	FD_ZERO(&in_read);
	FD_ZERO(&in_write);
	FD_ZERO(&in_except);
	if (read_set)
		in_read = *read_set;
	if (write_set)
		in_write = *write_set;
	if (except_set)
		in_except = *except_set;

	struct pollfd pfds_stack[64];
	struct pollfd *pfds = pfds_stack;
	if (num_fds > 64) {
		pfds = reinterpret_cast<struct pollfd *>(
		    malloc((size_t)num_fds * sizeof(struct pollfd)));
		if (!pfds)
			return ENOMEM;
	}

	nfds_t n = 0;
	for (int fd = 0; fd < num_fds; fd++) {
		bool r = FD_ISSET(fd, &in_read);
		bool w = FD_ISSET(fd, &in_write);
		bool x = FD_ISSET(fd, &in_except);
		if (!r && !w && !x)
			continue;
		pfds[n].fd = fd;
		// An except-only entry registers with zero interests; error/closed
		// events are delivered regardless.
		pfds[n].events = (short)((r ? POLLIN : 0) | (w ? POLLOUT : 0));
		pfds[n].revents = 0;
		n++;
	}

	uint64_t ns = UINT64_MAX;
	if (timeout)
		ns = (uint64_t)timeout->tv_sec * 1000000000ull + (uint64_t)timeout->tv_nsec;

	int ignored;
	int e = do_poll(pfds, n, ns, &ignored);
	if (e) {
		if (pfds != pfds_stack)
			free(pfds);
		return e;
	}

	if (read_set)
		FD_ZERO(read_set);
	if (write_set)
		FD_ZERO(write_set);
	if (except_set)
		FD_ZERO(except_set);

	int bits = 0;
	int err = 0;
	for (nfds_t i = 0; i < n; i++) {
		if (pfds[i].revents & POLLNVAL) { // select() semantics: EBADF, not a bit
			err = EBADF;
			break;
		}
		int fd = pfds[i].fd;
		if (FD_ISSET(fd, &in_read)
		    && (pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
			FD_SET(fd, read_set);
			bits++;
		}
		if (FD_ISSET(fd, &in_write) && (pfds[i].revents & (POLLOUT | POLLERR))) {
			FD_SET(fd, write_set);
			bits++;
		}
		if (FD_ISSET(fd, &in_except) && (pfds[i].revents & POLLERR)) {
			FD_SET(fd, except_set);
			bits++;
		}
	}

	if (pfds != pfds_stack)
		free(pfds);
	if (err)
		return err;
	*num_events = bits;
	return 0;
}

} // namespace mlibc
