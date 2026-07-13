// Motor OS BSD-socket bring-up: the pseudo-socket table bridging BSD's
// socket()-then-bind/connect lifecycle onto Motor's create-bound-or-connected
// net API. Design: docs/porting-libc-appendix-g.md (G.3).

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <fcntl.h>
#include <frg/mutex.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/lock.hpp>
#include <mlibc/motor-socket.hpp>
#include <mlibc/motor-util.hpp>
#include <moto_rt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

namespace {

using mlibc::MOTOR_PSEUDO_FD_BASE;
using mlibc::moto_to_errno;

constexpr int MAX_PSEUDO_SOCKETS = 128;

enum class SockState : uint8_t {
	kFresh,      // socket() done, nothing Motor-side
	kConnecting, // connect() in flight (lock dropped during the Motor call)
	kBound,      // bind() done -> real_fd valid
	kConnected,  // connect() done -> real_fd valid
	kListening,  // listen() done -> real_fd valid
};

struct PseudoSocket {
	bool in_use;
	int family; // AF_INET / AF_INET6 (Linux values)
	int type;   // SOCK_STREAM / SOCK_DGRAM (flags stripped)
	bool nonblocking;
	// Listener async-accept machinery armed (VDSO listen() called). Listener
	// readiness events only fire when armed (appendix H.4.4), and arming
	// requires the VDSO-level socket to be nonblocking — so once armed, the
	// VDSO-level mode is pinned nonblocking regardless of ps->nonblocking,
	// and a blocking accept() is emulated in Sysdeps<Accept>.
	bool armed;
	SockState state;
	int real_fd;      // -1 until materialized
	uint32_t backlog; // listen() backlog, kept for poll-time arming
};

PseudoSocket psock_table[MAX_PSEUDO_SOCKETS];
FutexLock psock_lock;

// Caller holds psock_lock.
PseudoSocket *psock_get(int fd) {
	int idx = fd - MOTOR_PSEUDO_FD_BASE;
	if (idx < 0 || idx >= MAX_PSEUDO_SOCKETS || !psock_table[idx].in_use)
		return nullptr;
	return &psock_table[idx];
}

// ---- address translation: Linux ABI <-> moto netc (G.3.2) -----------------

int lx_to_moto_addr(const struct sockaddr *addr, socklen_t len, moto_sockaddr_t *out) {
	if (!addr)
		return EDESTADDRREQ;
	if (addr->sa_family == AF_INET && len >= (socklen_t)sizeof(struct sockaddr_in)) {
		auto *in4 = reinterpret_cast<const struct sockaddr_in *>(addr);
		memset(out, 0, sizeof(*out));
		out->v4.family = MOTO_AF_INET;
		out->v4.port = in4->sin_port;           // big-endian both sides: copy as-is
		out->v4.addr = in4->sin_addr.s_addr;    // ditto
		return 0;
	}
	if (addr->sa_family == AF_INET6 && len >= (socklen_t)sizeof(struct sockaddr_in6)) {
		auto *in6 = reinterpret_cast<const struct sockaddr_in6 *>(addr);
		memset(out, 0, sizeof(*out));
		out->v6.family = MOTO_AF_INET6;
		out->v6.port = in6->sin6_port;
		memcpy(out->v6.addr, in6->sin6_addr.s6_addr, 16);
		out->v6.flowinfo = in6->sin6_flowinfo;
		out->v6.scope_id = in6->sin6_scope_id;
		return 0;
	}
	return EAFNOSUPPORT;
}

// Honors the caller's capacity (*len in), reports the full size (POSIX).
void moto_to_lx_addr(const moto_sockaddr_t *m, struct sockaddr *addr, socklen_t *len) {
	if (!addr || !len)
		return;
	socklen_t cap = *len;
	if (m->v4.family == MOTO_AF_INET) {
		struct sockaddr_in out;
		memset(&out, 0, sizeof(out));
		out.sin_family = AF_INET;
		out.sin_port = m->v4.port;
		out.sin_addr.s_addr = m->v4.addr;
		*len = sizeof(out);
		memcpy(addr, &out, cap < sizeof(out) ? cap : sizeof(out));
	} else {
		struct sockaddr_in6 out;
		memset(&out, 0, sizeof(out));
		out.sin6_family = AF_INET6;
		out.sin6_port = m->v6.port;
		memcpy(out.sin6_addr.s6_addr, m->v6.addr, 16);
		out.sin6_flowinfo = m->v6.flowinfo;
		out.sin6_scope_id = m->v6.scope_id;
		*len = sizeof(out);
		memcpy(addr, &out, cap < sizeof(out) ? cap : sizeof(out));
	}
}

// Caller holds psock_lock. Auto-bind an unbound UDP socket to the any-address
// with an ephemeral port (POSIX auto-bind semantics).
int materialize_udp(PseudoSocket *ps) {
	__ensure(ps->type == SOCK_DGRAM && ps->real_fd < 0);
	moto_sockaddr_t any;
	memset(&any, 0, sizeof(any));
	any.v4.family = ps->family == AF_INET ? MOTO_AF_INET : MOTO_AF_INET6;
	int64_t r = moto_rt_net_bind(MOTO_PROTO_UDP, &any);
	if (r < 0)
		return moto_to_errno(r);
	ps->real_fd = static_cast<int>(r);
	ps->state = SockState::kBound;
	if (ps->nonblocking)
		moto_rt_net_set_nonblocking(ps->real_fd, 1);
	return 0;
}

// Resolve fd for a socket-specific op. Returns 0 and fills *real/*ps_type, or
// a positive errno. dgram_autobind: bind unbound UDP sockets on first use.
int resolve_for_io(int fd, int *real, int *ps_type, bool dgram_autobind) {
	if (fd < MOTOR_PSEUDO_FD_BASE) {
		*real = fd;
		*ps_type = SOCK_STREAM; // real fds we hand out are TCP (accept())
		return 0;
	}
	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return EBADF;
	*ps_type = ps->type;
	if (ps->real_fd < 0) {
		if (ps->type == SOCK_DGRAM && dgram_autobind) {
			if (int e = materialize_udp(ps))
				return e;
		} else {
			return ENOTCONN;
		}
	}
	*real = ps->real_fd;
	return 0;
}

void log_once(const char *what) {
	static int logged; // per-message would be nicer; one flag is enough for now
	if (!__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
		mlibc::infoLogger() << "mlibc/motor sockets: " << what << frg::endlog;
}

} // namespace

namespace mlibc {

int motor_sock_realfd(int fd) {
	int real, type;
	if (int e = resolve_for_io(fd, &real, &type, /*dgram_autobind=*/true))
		return -e;
	return real;
}

int motor_sock_pollfd(int fd) {
	if (fd < MOTOR_PSEUDO_FD_BASE)
		return fd;
	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return -EBADF;
	if (ps->real_fd < 0) {
		if (ps->type != SOCK_DGRAM)
			return -EAGAIN; // fresh TCP socket: nothing to poll, never ready
		if (int e = materialize_udp(ps))
			return -e;
	}
	// Listener readiness events only fire once the async-accept machinery is
	// armed (VDSO listen(), which requires VDSO-level nonblocking) — H.4.4.
	if (ps->state == SockState::kListening && !ps->armed) {
		moto_rt_net_set_nonblocking(ps->real_fd, 1);
		int32_t r = moto_rt_net_listen(ps->real_fd, ps->backlog);
		if (r < 0) {
			moto_rt_net_set_nonblocking(ps->real_fd, ps->nonblocking ? 1 : 0);
			return -moto_to_errno(r);
		}
		ps->armed = true;
	}
	return ps->real_fd;
}

int motor_sock_close(int fd) {
	if (fd < MOTOR_PSEUDO_FD_BASE)
		return -1;
	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return EBADF;
	int real = ps->real_fd;
	ps->in_use = false;
	ps->real_fd = -1;
	guard.unlock();
	if (real >= 0)
		return moto_to_errno(moto_rt_close(real));
	return 0;
}

int Sysdeps<Socket>::operator()(int family, int type, int protocol, int *fd) {
	int flags = type & ~0xF;
	type &= 0xF;
	if (family != AF_INET && family != AF_INET6)
		return EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return EPROTONOSUPPORT;
	if (protocol && !((type == SOCK_STREAM && protocol == IPPROTO_TCP)
	                  || (type == SOCK_DGRAM && protocol == IPPROTO_UDP)))
		return EPROTONOSUPPORT;
	if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
		return EINVAL;

	frg::unique_lock guard{psock_lock};
	for (int i = 0; i < MAX_PSEUDO_SOCKETS; i++) {
		auto *ps = &psock_table[i];
		if (ps->in_use)
			continue;
		ps->in_use = true;
		ps->family = family;
		ps->type = type;
		ps->nonblocking = flags & SOCK_NONBLOCK;
		ps->armed = false;
		ps->state = SockState::kFresh;
		ps->real_fd = -1;
		ps->backlog = 16;
		*fd = MOTOR_PSEUDO_FD_BASE + i;
		return 0;
	}
	return ENFILE;
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr *addr_ptr,
                              socklen_t addr_length) {
	moto_sockaddr_t ma;
	if (int e = lx_to_moto_addr(addr_ptr, addr_length, &ma))
		return e;
	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return fd < MOTOR_PSEUDO_FD_BASE ? EINVAL : EBADF;
	if (ps->state != SockState::kFresh)
		return EINVAL;
	if ((ps->family == AF_INET) != (ma.v4.family == MOTO_AF_INET))
		return EAFNOSUPPORT;
	int64_t r = moto_rt_net_bind(
	    ps->type == SOCK_STREAM ? MOTO_PROTO_TCP : MOTO_PROTO_UDP, &ma);
	if (r < 0)
		return moto_to_errno(r);
	ps->real_fd = static_cast<int>(r);
	ps->state = SockState::kBound;
	if (ps->nonblocking)
		moto_rt_net_set_nonblocking(ps->real_fd, 1);
	return 0;
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr *addr_ptr,
                                 socklen_t addr_length) {
	moto_sockaddr_t ma;
	if (int e = lx_to_moto_addr(addr_ptr, addr_length, &ma))
		return e;

	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return fd < MOTOR_PSEUDO_FD_BASE ? EISCONN : EBADF;

	if (ps->type == SOCK_STREAM) {
		switch (ps->state) {
		case SockState::kFresh:
			break;
		case SockState::kConnecting:
			return EALREADY;
		case SockState::kBound:
		case SockState::kListening:
			// Motor's tcp_connect *creates* the socket; a client bound to a
			// fixed source port is not expressible (G.6).
			return EOPNOTSUPP;
		case SockState::kConnected:
			return EISCONN;
		}
		// tcp_connect can block for a long time: drop the table lock, hold
		// the slot with kConnecting instead.
		ps->state = SockState::kConnecting;
		bool nonblocking = ps->nonblocking;
		guard.unlock();

		int64_t r = moto_rt_net_tcp_connect(&ma, UINT64_MAX, nonblocking ? 1 : 0);

		guard.lock();
		ps = psock_get(fd); // revalidate (close() may have raced us)
		if (!ps)
			return EBADF;
		if (r < 0) {
			ps->state = SockState::kFresh;
			return moto_to_errno(r);
		}
		ps->real_fd = static_cast<int>(r);
		ps->state = SockState::kConnected;
		return 0;
	}

	// UDP: auto-bind if fresh, then connect the bound socket.
	if (ps->real_fd < 0) {
		if (int e = materialize_udp(ps))
			return e;
	}
	int32_t r = moto_rt_net_udp_connect(ps->real_fd, &ma);
	if (r < 0)
		return moto_to_errno(r);
	ps->state = SockState::kConnected;
	return 0;
}

int Sysdeps<Listen>::operator()(int fd, int backlog) {
	frg::unique_lock guard{psock_lock};
	auto *ps = psock_get(fd);
	if (!ps)
		return fd < MOTOR_PSEUDO_FD_BASE ? EINVAL : EBADF;
	if (ps->type != SOCK_STREAM)
		return EOPNOTSUPP;
	if (ps->state != SockState::kBound)
		return ps->state == SockState::kListening ? 0 : EDESTADDRREQ;
	if (backlog <= 0)
		backlog = 16;
	ps->backlog = static_cast<uint32_t>(backlog);
	// Motor's bind(PROTO_TCP) already yields an accept()able listener; the
	// VDSO's listen() only arms the async-accept machinery and *requires* the
	// socket to be nonblocking (rt_tcp.rs:250 returns InvalidArgument else).
	// So for blocking sockets listen() is bookkeeping-only here; poll() arms
	// on demand via motor_sock_pollfd().
	if (ps->nonblocking) {
		int32_t r = moto_rt_net_listen(ps->real_fd, ps->backlog);
		if (r < 0)
			return moto_to_errno(r);
		ps->armed = true;
	}
	ps->state = SockState::kListening;
	return 0;
}

int Sysdeps<Accept>::operator()(int fd, int *newfd, struct sockaddr *addr_ptr,
                                socklen_t *addr_length, int flags) {
	if (flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC))
		return EINVAL;
	int real;
	bool emulate_blocking = false;
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		frg::unique_lock guard{psock_lock};
		auto *ps = psock_get(fd);
		if (!ps)
			return EBADF;
		if (ps->real_fd < 0)
			return EINVAL; // not bound/listening
		real = ps->real_fd;
		// Armed listener = VDSO-level nonblocking; if the app-visible mode is
		// blocking, emulate: retry EAGAIN after waiting for readability.
		emulate_blocking = ps->armed && !ps->nonblocking;
	} else {
		real = fd;
	}

	moto_sockaddr_t peer;
	int64_t r;
	for (;;) {
		memset(&peer, 0, sizeof(peer));
		r = moto_rt_net_accept(real, &peer);
		if (!emulate_blocking || r != -MOTO_E_NOT_READY)
			break;
		// Rare path: poll said readable but another thread raced us, or the
		// caller skipped poll entirely. Park on a throwaway registry.
		int pfd = moto_rt_poll_new();
		if (pfd < 0)
			return moto_to_errno(pfd);
		int32_t e = moto_rt_poll_add(pfd, real, 0, MOTO_POLL_READABLE);
		if (e < 0) {
			moto_rt_close(pfd);
			return moto_to_errno(e);
		}
		moto_poll_event_t ev;
		int32_t n = moto_rt_poll_wait(pfd, UINT64_MAX, &ev, 1);
		moto_rt_poll_del(pfd, real);
		moto_rt_close(pfd);
		if (n < 0)
			return moto_to_errno(n);
	}
	if (r < 0)
		return moto_to_errno(r);

	// The accepted fd is fully materialized: hand out the real fd directly.
	*newfd = static_cast<int>(r);
	if (flags & SOCK_NONBLOCK)
		moto_rt_net_set_nonblocking(*newfd, 1);
	if (addr_ptr && addr_length)
		moto_to_lx_addr(&peer, addr_ptr, addr_length);
	return 0;
}

int Sysdeps<Sendto>::operator()(int fd, const void *buffer, size_t size, int flags,
                                const struct sockaddr *sock_addr,
                                socklen_t addr_length, ssize_t *length) {
	flags &= ~MSG_NOSIGNAL; // no signals on Motor; error returns suffice
	if (flags) {
		log_once("unsupported send flags");
		return EINVAL;
	}
	int real, type;
	if (int e = resolve_for_io(fd, &real, &type, /*dgram_autobind=*/true))
		return e;

	int64_t r;
	if (type == SOCK_DGRAM && sock_addr && addr_length) {
		moto_sockaddr_t ma;
		if (int e = lx_to_moto_addr(sock_addr, addr_length, &ma))
			return e;
		r = moto_rt_net_udp_send_to(real, reinterpret_cast<const uint8_t *>(buffer),
		                            size, &ma);
	} else {
		// TCP (address ignored per POSIX) or connected UDP.
		r = moto_rt_write(real, reinterpret_cast<const uint8_t *>(buffer), size);
	}
	if (r < 0)
		return moto_to_errno(r);
	*length = r;
	return 0;
}

int Sysdeps<Recvfrom>::operator()(int fd, void *buffer, size_t size, int flags,
                                  struct sockaddr *sock_addr, socklen_t *addr_length,
                                  ssize_t *length) {
	bool peek = flags & MSG_PEEK;
	flags &= ~MSG_PEEK;
	if (flags) {
		log_once("unsupported recv flags");
		return EINVAL;
	}
	int real, type;
	if (int e = resolve_for_io(fd, &real, &type, /*dgram_autobind=*/true))
		return e;

	int64_t r;
	if (type == SOCK_DGRAM) {
		moto_sockaddr_t from;
		memset(&from, 0, sizeof(from));
		r = peek ? moto_rt_net_udp_peek_from(real, reinterpret_cast<uint8_t *>(buffer),
		                                     size, &from)
		         : moto_rt_net_udp_recv_from(real, reinterpret_cast<uint8_t *>(buffer),
		                                     size, &from);
		if (r >= 0 && sock_addr && addr_length)
			moto_to_lx_addr(&from, sock_addr, addr_length);
	} else {
		r = peek ? moto_rt_net_peek(real, reinterpret_cast<uint8_t *>(buffer), size)
		         : moto_rt_read(real, reinterpret_cast<uint8_t *>(buffer), size);
		// Connection-oriented: the source address is not meaningful.
		if (sock_addr && addr_length)
			*addr_length = 0;
	}
	if (r < 0)
		return moto_to_errno(r);
	*length = r;
	return 0;
}

int Sysdeps<Shutdown>::operator()(int sockfd, int how) {
	int real, type;
	if (int e = resolve_for_io(sockfd, &real, &type, /*dgram_autobind=*/false))
		return e;
	uint8_t moto_how;
	switch (how) { // Linux SHUT_RD/WR/RDWR = 0/1/2; Motor READ/WRITE = 1/2
	case SHUT_RD:   moto_how = MOTO_SHUTDOWN_READ; break;
	case SHUT_WR:   moto_how = MOTO_SHUTDOWN_WRITE; break;
	case SHUT_RDWR: moto_how = MOTO_SHUTDOWN_READ | MOTO_SHUTDOWN_WRITE; break;
	default:        return EINVAL;
	}
	return moto_to_errno(moto_rt_net_shutdown(real, moto_how));
}

int Sysdeps<Sockname>::operator()(int fd, struct sockaddr *addr_ptr,
                                  socklen_t max_addr_length,
                                  socklen_t *actual_length) {
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		frg::unique_lock guard{psock_lock};
		auto *ps = psock_get(fd);
		if (!ps)
			return EBADF;
		if (ps->real_fd < 0) {
			// Unbound: the all-zero address of the right family (POSIX).
			moto_sockaddr_t zero;
			memset(&zero, 0, sizeof(zero));
			zero.v4.family = ps->family == AF_INET ? MOTO_AF_INET : MOTO_AF_INET6;
			socklen_t len = max_addr_length;
			moto_to_lx_addr(&zero, addr_ptr, &len);
			*actual_length = len;
			return 0;
		}
		fd = ps->real_fd;
	}
	moto_sockaddr_t ma;
	int32_t r = moto_rt_net_socket_addr(fd, &ma);
	if (r < 0)
		return moto_to_errno(r);
	socklen_t len = max_addr_length;
	moto_to_lx_addr(&ma, addr_ptr, &len);
	*actual_length = len;
	return 0;
}

int Sysdeps<Peername>::operator()(int fd, struct sockaddr *addr_ptr,
                                  socklen_t max_addr_length,
                                  socklen_t *actual_length) {
	int real, type;
	if (int e = resolve_for_io(fd, &real, &type, /*dgram_autobind=*/false))
		return e == ENOTCONN ? ENOTCONN : e;
	moto_sockaddr_t ma;
	int32_t r = moto_rt_net_peer_addr(real, &ma);
	if (r < 0)
		return moto_to_errno(r);
	socklen_t len = max_addr_length;
	moto_to_lx_addr(&ma, addr_ptr, &len);
	*actual_length = len;
	return 0;
}

namespace {

// Options acting on real fds; ps may be null for real (accepted) fds.
int do_setsockopt(int real, int layer, int number, const void *buffer, socklen_t size) {
	if (layer == SOL_SOCKET) {
		switch (number) {
		case SO_REUSEADDR:
		case SO_KEEPALIVE:
			return 0; // accepted and ignored (G.3.3)
		case SO_BROADCAST: {
			if (size < sizeof(int))
				return EINVAL;
			int v = *reinterpret_cast<const int *>(buffer);
			return moto_to_errno(moto_rt_net_set_broadcast(real, v ? 1 : 0));
		}
		case SO_RCVTIMEO:
		case SO_SNDTIMEO: {
			if (size < sizeof(struct timeval))
				return EINVAL;
			auto *tv = reinterpret_cast<const struct timeval *>(buffer);
			uint64_t nanos;
			if (tv->tv_sec == 0 && tv->tv_usec == 0)
				nanos = UINT64_MAX; // {0,0} = no timeout
			else
				nanos = static_cast<uint64_t>(tv->tv_sec) * 1000000000ul
				        + static_cast<uint64_t>(tv->tv_usec) * 1000ul;
			return moto_to_errno(number == SO_RCVTIMEO
			                         ? moto_rt_net_set_read_timeout(real, nanos)
			                         : moto_rt_net_set_write_timeout(real, nanos));
		}
		default:
			break;
		}
	} else if (layer == IPPROTO_TCP && number == TCP_NODELAY) {
		if (size < sizeof(int))
			return EINVAL;
		int v = *reinterpret_cast<const int *>(buffer);
		return moto_to_errno(moto_rt_net_set_nodelay(real, v ? 1 : 0));
	} else if (layer == IPPROTO_IP && number == IP_TTL) {
		if (size < sizeof(int))
			return EINVAL;
		int v = *reinterpret_cast<const int *>(buffer);
		return moto_to_errno(moto_rt_net_set_ttl(real, static_cast<uint32_t>(v)));
	}
	log_once("unsupported setsockopt option");
	return ENOPROTOOPT;
}

int do_getsockopt(int real, int layer, int number, void *buffer, socklen_t *size) {
	if (*size < (socklen_t)sizeof(int))
		return EINVAL;
	int *out = reinterpret_cast<int *>(buffer);
	if (layer == SOL_SOCKET) {
		switch (number) {
		case SO_REUSEADDR:
		case SO_KEEPALIVE:
			*out = 0;
			*size = sizeof(int);
			return 0;
		case SO_ERROR: {
			int32_t r = moto_rt_net_take_error(real);
			*out = r == 0 ? 0 : moto_to_errno(r);
			*size = sizeof(int);
			return 0;
		}
		case SO_BROADCAST: {
			int32_t r = moto_rt_net_broadcast(real);
			if (r < 0)
				return moto_to_errno(r);
			*out = r;
			*size = sizeof(int);
			return 0;
		}
		case SO_RCVTIMEO:
		case SO_SNDTIMEO: {
			if (*size < (socklen_t)sizeof(struct timeval))
				return EINVAL;
			int64_t nanos = number == SO_RCVTIMEO ? moto_rt_net_read_timeout(real)
			                                      : moto_rt_net_write_timeout(real);
			if (nanos < 0)
				return moto_to_errno(nanos);
			auto *tv = reinterpret_cast<struct timeval *>(buffer);
			if (nanos == INT64_MAX) { // none
				tv->tv_sec = 0;
				tv->tv_usec = 0;
			} else {
				tv->tv_sec = nanos / 1000000000l;
				tv->tv_usec = (nanos % 1000000000l) / 1000l;
			}
			*size = sizeof(struct timeval);
			return 0;
		}
		default:
			break;
		}
	} else if (layer == IPPROTO_TCP && number == TCP_NODELAY) {
		int32_t r = moto_rt_net_nodelay(real);
		if (r < 0)
			return moto_to_errno(r);
		*out = r;
		*size = sizeof(int);
		return 0;
	} else if (layer == IPPROTO_IP && number == IP_TTL) {
		int64_t r = moto_rt_net_ttl(real);
		if (r < 0)
			return moto_to_errno(r);
		*out = static_cast<int>(r);
		*size = sizeof(int);
		return 0;
	}
	log_once("unsupported getsockopt option");
	return ENOPROTOOPT;
}

} // namespace

int Sysdeps<SetSockopt>::operator()(int fd, int layer, int number, const void *buffer,
                                    socklen_t size) {
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		frg::unique_lock guard{psock_lock};
		auto *ps = psock_get(fd);
		if (!ps)
			return EBADF;
		if (ps->real_fd < 0) {
			// Pre-materialization only the no-op options make sense.
			if (layer == SOL_SOCKET
			    && (number == SO_REUSEADDR || number == SO_KEEPALIVE))
				return 0;
			return EINVAL;
		}
		fd = ps->real_fd;
	}
	return do_setsockopt(fd, layer, number, buffer, size);
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number, void *buffer,
                                    socklen_t *size) {
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		frg::unique_lock guard{psock_lock};
		auto *ps = psock_get(fd);
		if (!ps)
			return EBADF;
		if (ps->real_fd < 0) {
			if (*size >= (socklen_t)sizeof(int)
			    && (layer != SOL_SOCKET
			        || (number == SO_REUSEADDR || number == SO_KEEPALIVE
			            || number == SO_ERROR))) {
				*reinterpret_cast<int *>(buffer) = 0;
				*size = sizeof(int);
				return 0;
			}
			return EINVAL;
		}
		fd = ps->real_fd;
	}
	return do_getsockopt(fd, layer, number, buffer, size);
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	switch (request) {
	case F_GETFD:
		*result = 0;
		return 0;
	case F_SETFD:
		*result = 0; // FD_CLOEXEC is meaningless without exec
		return 0;
	case F_GETFL: {
		int fl = O_RDWR;
		if (fd >= MOTOR_PSEUDO_FD_BASE) {
			frg::unique_lock guard{psock_lock};
			auto *ps = psock_get(fd);
			if (!ps)
				return EBADF;
			if (ps->nonblocking)
				fl |= O_NONBLOCK;
		}
		*result = fl;
		return 0;
	}
	case F_SETFL: {
		int flags = va_arg(args, int);
		bool nb = flags & O_NONBLOCK;
		if (fd >= MOTOR_PSEUDO_FD_BASE) {
			frg::unique_lock guard{psock_lock};
			auto *ps = psock_get(fd);
			if (!ps)
				return EBADF;
			ps->nonblocking = nb;
			// An armed listener's VDSO-level mode stays pinned nonblocking
			// (disarming would kill its readiness events); blocking accept()
			// is emulated instead.
			if (ps->real_fd >= 0 && !ps->armed)
				moto_rt_net_set_nonblocking(ps->real_fd, nb ? 1 : 0);
		} else {
			// Best effort: works on real socket fds (accept results); fails
			// harmlessly on files, whose flags we don't track.
			moto_rt_net_set_nonblocking(fd, nb ? 1 : 0);
		}
		*result = 0;
		return 0;
	}
	default:
		mlibc::infoLogger() << "mlibc/motor: fcntl(" << request << ") is unsupported"
		                    << frg::endlog;
		return ENOSYS;
	}
}

} // namespace mlibc
