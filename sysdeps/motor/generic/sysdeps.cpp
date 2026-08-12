// Motor OS sysdeps: thin wrappers over the moto-rt-cabi shim (moto_rt.h),
// which proxies the Motor OS RT.VDSO. See motor-os docs/porting-libc-by-fable.md.

#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/vm-flags.h>
#include <bits/ensure.h>
#include <dirent.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/motor-socket.hpp> // pseudo-socket fd hooks (see socket.cpp)
#include <mlibc/motor-util.hpp>   // moto_to_errno
#include <moto_rt.h>
#include <stddef.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

using mlibc::moto_to_errno;

namespace {

static_assert(sizeof(moto_file_attr_t) == 96); // v2: + entry_id
static_assert(sizeof(moto_dir_entry_t) == 384); // v2

// Motor FileAttr -> struct stat. Motor has no inode numbers, uids, or links;
// st_ino comes from the motor-fs entry id (FileAttr v2): the low u64 is the
// entry's block_no — unique among live entries, stable for the entry's
// lifetime, reused after deletion — i.e. classic Unix inode semantics. +1
// keeps root (block 0) away from st_ino==0, which some tools treat as
// "deleted". The high u64 (generation, an ABA guard) has no stat field and
// is dropped. Same-file detection via (st_dev, st_ino) works since v2;
// clang's FileManager was the first real consumer (appendix J pitfalls).
int attr_to_stat(const moto_file_attr_t *a, struct stat *st) {
	memset(st, 0, sizeof(*st));
	st->st_nlink = 1;
	st->st_blksize = 4096;

	mode_t mode = 0;
	switch (a->file_type) {
	case MOTO_FILETYPE_FILE:
		mode = S_IFREG;
		break;
	case MOTO_FILETYPE_DIRECTORY:
		mode = S_IFDIR | 0111; // directories are traversable
		break;
	case MOTO_FILETYPE_CHARACTER_DEVICE:
		mode = S_IFCHR | 0600;
		break;
	case MOTO_FILETYPE_FIFO:
		mode = S_IFIFO | 0600;
		break;
	case MOTO_FILETYPE_SOCKET:
		mode = S_IFSOCK | 0600;
		break;
	case MOTO_FILETYPE_ANONYMOUS:
		mode = 0600;
		break;
	default:
		return EIO;
	}

	if (a->file_type >= MOTO_FILETYPE_CHARACTER_DEVICE) {
		if (!a->entry_id_lo || a->entry_id_hi)
			return EIO;
		st->st_dev = 2;
		st->st_ino = static_cast<ino_t>(a->entry_id_lo);
		st->st_mode = mode;
		return 0;
	}

	st->st_dev = 1;
	st->st_ino = static_cast<ino_t>(a->entry_id_lo + 1);
	if (a->perm & MOTO_PERM_READ)
		mode |= 0444;
	if (a->perm & MOTO_PERM_WRITE)
		mode |= 0222;
	st->st_mode = mode;
	st->st_size = static_cast<off_t>(a->size);
	st->st_blocks = static_cast<blkcnt_t>((a->size + 511) / 512);
	// u128 nanos since the UNIX epoch; the high half is always 0 for
	// realistic dates (u64 nanos reach the year 2554). 0 stays 0 (unknown).
	auto to_ts = [](uint64_t nanos) {
		struct timespec ts;
		ts.tv_sec = static_cast<time_t>(nanos / 1000000000ul);
		ts.tv_nsec = static_cast<long>(nanos % 1000000000ul);
		return ts;
	};
	st->st_atim = to_ts(a->accessed_lo);
	st->st_mtim = to_ts(a->modified_lo);
	st->st_ctim = to_ts(a->modified_lo); // no status-change time on Motor
	return 0;
}

} // namespace

namespace mlibc {

void Sysdeps<LibcLog>::operator()(const char *msg) {
	moto_rt_log(reinterpret_cast<const uint8_t *>(msg), strlen(msg));
}

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("!!! mlibc panic !!!");
	moto_rt_proc_exit(-1);
}

void Sysdeps<Exit>::operator()(int status) { moto_rt_proc_exit(status); }

int Sysdeps<TcbSet>::operator()(void *pointer) {
	moto_rt_tcb_set(pointer); // UTCB.libc_tcb (fs:0x58); read by get_current_tcb()
	return 0;
}

pid_t Sysdeps<FutexTid>::operator()() { return static_cast<pid_t>(moto_rt_tid()); }

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *time) {
	uint64_t timeout = UINT64_MAX; // no timeout
	if (time)
		timeout = static_cast<uint64_t>(time->tv_sec) * 1000000000ul
		        + static_cast<uint64_t>(time->tv_nsec);
	int woken = moto_rt_futex_wait(
	    reinterpret_cast<const uint32_t *>(pointer), static_cast<uint32_t>(expected), timeout);
	if (!woken && time)
		return ETIMEDOUT;
	return 0;
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
	if (all)
		moto_rt_futex_wake_all(reinterpret_cast<const uint32_t *>(pointer));
	else
		moto_rt_futex_wake(reinterpret_cast<const uint32_t *>(pointer));
	return 0;
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	int64_t r = moto_rt_vm_map(size);
	if (r < 0)
		return moto_to_errno(r);
	*pointer = reinterpret_cast<void *>(r);
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t) {
	return moto_to_errno(moto_rt_vm_unmap(reinterpret_cast<uint64_t>(pointer)));
}

int Sysdeps<VmMap>::operator()(void *, size_t size, int, int flags, int fd, off_t, void **window) {
	if (!(flags & MAP_ANONYMOUS) || fd != -1)
		return ENOSYS; // no file-backed mmap on Motor (platform property)
	return sysdep<AnonAllocate>(size, window);
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	return sysdep<AnonFree>(pointer, size);
}

int Sysdeps<Open>::operator()(const char *pathname, int flags, mode_t, int *fd) {
	uint32_t opts = 0;
	switch (flags & O_ACCMODE) {
	case O_RDONLY: opts = MOTO_O_READ; break;
	case O_WRONLY: opts = MOTO_O_WRITE; break;
	case O_RDWR:   opts = MOTO_O_READ | MOTO_O_WRITE; break;
	default:       return EINVAL;
	}
	if (flags & O_APPEND)   opts |= MOTO_O_APPEND;
	if (flags & O_TRUNC)    opts |= MOTO_O_TRUNCATE;
	if (flags & O_CREAT)    opts |= MOTO_O_CREATE;
	if (flags & O_EXCL)     opts |= MOTO_O_CREATE_NEW;
	if (flags & O_NONBLOCK) opts |= MOTO_O_NONBLOCK;
	int64_t r = moto_rt_open(reinterpret_cast<const uint8_t *>(pathname),
	                         strlen(pathname), opts);
	if (r < 0)
		return moto_to_errno(r);
	*fd = static_cast<int>(r);
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		fd = motor_sock_realfd(fd);
		if (fd < 0)
			return -fd;
	}
	int64_t r = moto_rt_read(fd, reinterpret_cast<uint8_t *>(buf), count);
	if (r < 0)
		return moto_to_errno(r);
	*bytes_read = r;
	return 0;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
	if (fd >= MOTOR_PSEUDO_FD_BASE) {
		fd = motor_sock_realfd(fd);
		if (fd < 0)
			return -fd;
	}
	int64_t r = moto_rt_write(fd, reinterpret_cast<const uint8_t *>(buf), count);
	if (r < 0)
		return moto_to_errno(r);
	*bytes_written = r;
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	uint8_t w;
	switch (whence) {
	case SEEK_SET: w = MOTO_SEEK_SET; break;
	case SEEK_CUR: w = MOTO_SEEK_CUR; break;
	case SEEK_END: w = MOTO_SEEK_END; break;
	default:       return EINVAL;
	}
	int64_t r = moto_rt_seek(fd, offset, w);
	if (r < 0) {
		// The VDSO's seek returns BadHandle (17) for any open fd that is not
		// a regular file (rt.vdso/src/rt_fs.rs downcast) — for the stdio fds,
		// which always exist on Motor, that means "non-seekable stream", which
		// POSIX (and mlibc's fd_file::determine_type) spells ESPIPE.
		if (-r == 17 /* BadHandle */ && fd >= 0 && fd <= 2)
			return ESPIPE;
		return moto_to_errno(r);
	}
	*new_offset = r;
	return 0;
}

int Sysdeps<Close>::operator()(int fd) {
	int r = motor_sock_close(fd);
	if (r >= 0) // it was a pseudo-socket; the slot (and real fd) are gone
		return r;
	return moto_to_errno(moto_rt_close(fd));
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	// The VDSO's rmdir currently aliases unlink and deletes any entry kind,
	// so remove() works on files through this path alone (its ENOTDIR->
	// Unlinkat fallback never fires on Motor). Cost: rmdir() on a file does
	// not fail with ENOTDIR as POSIX wants. Tighten when Stat lands (M4).
	return moto_to_errno(
	    moto_rt_rmdir(reinterpret_cast<const uint8_t *>(path), strlen(path)));
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
	if (dirfd != AT_FDCWD && path[0] != '/')
		return EBADF; // no dirfd-relative resolution on Motor (openat is M4+)
	if (flags & AT_REMOVEDIR)
		return sysdep<Rmdir>(path);
	if (flags)
		return EINVAL;
	return moto_to_errno(
	    moto_rt_unlink(reinterpret_cast<const uint8_t *>(path), strlen(path)));
}

int Sysdeps<Rename>::operator()(const char *path, const char *new_path) {
	return moto_to_errno(moto_rt_rename(
	    reinterpret_cast<const uint8_t *>(path), strlen(path),
	    reinterpret_cast<const uint8_t *>(new_path), strlen(new_path)));
}

int Sysdeps<Stat>::operator()(fsfd_target fsfdt, int fd, const char *path, int flags,
                              struct stat *result) {
	// AT_SYMLINK_NOFOLLOW is accepted and ignored: Motor has no symlinks,
	// so follow/nofollow are the same operation.
	moto_file_attr_t attr;
	auto stat_fd = [&](int target_fd) {
		int socket_result = motor_sock_fstat(target_fd, result);
		if (socket_result != -1)
			return socket_result;
		int32_t r = moto_rt_fstat(target_fd, &attr);
		if (r < 0)
			return moto_to_errno(r);
		return attr_to_stat(&attr, result);
	};
	int32_t r;
	switch (fsfdt) {
	case fsfd_target::fd:
		return stat_fd(fd);
	case fsfd_target::path:
		r = moto_rt_stat(reinterpret_cast<const uint8_t *>(path), strlen(path), &attr);
		break;
	case fsfd_target::fd_path:
		if ((flags & AT_EMPTY_PATH) && !*path)
			return stat_fd(fd);
		if (fd != AT_FDCWD && path[0] != '/')
			return EBADF; // no dirfd-relative resolution on Motor
		r = moto_rt_stat(reinterpret_cast<const uint8_t *>(path), strlen(path), &attr);
		break;
	default:
		return EINVAL;
	}
	if (r < 0)
		return moto_to_errno(r);
	return attr_to_stat(&attr, result);
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	if (!size)
		return ERANGE;
	int64_t len = moto_rt_getcwd(reinterpret_cast<uint8_t *>(buffer), size - 1);
	if (len < 0)
		return moto_to_errno(len);
	if (static_cast<size_t>(len) + 1 > size)
		return ERANGE;
	buffer[len] = 0;
	return 0;
}

int Sysdeps<Chdir>::operator()(const char *path) {
	return moto_to_errno(
	    moto_rt_chdir(reinterpret_cast<const uint8_t *>(path), strlen(path)));
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t) {
	// mode ignored: Motor's perm model is per-entry r/w, no create-time mode.
	return moto_to_errno(
	    moto_rt_mkdir(reinterpret_cast<const uint8_t *>(path), strlen(path)));
}

int Sysdeps<Mkdirat>::operator()(int dirfd, const char *path, mode_t mode) {
	if (dirfd != AT_FDCWD && path[0] != '/')
		return EBADF;
	return sysdep<Mkdir>(path, mode);
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode,
                                int *fd) {
	if (dirfd != AT_FDCWD && path[0] != '/')
		return EBADF;
	return sysdep<Open>(path, flags, mode, fd);
}

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	int64_t r = moto_rt_opendir(reinterpret_cast<const uint8_t *>(path), strlen(path));
	if (r < 0)
		return moto_to_errno(r);
	*handle = static_cast<int>(r);
	return 0;
}

int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size,
                                     size_t *bytes_read) {
	// One dirent per call: Motor's readdir yields one entry at a time, and
	// packing more would require lookahead buffering (an entry pulled from
	// the server-side cursor can't be pushed back if it doesn't fit).
	// mlibc's readdir() copes fine: it re-calls when the buffer is consumed.
	moto_dir_entry_t ent;
	int32_t r = moto_rt_readdir(handle, &ent);
	if (r < 0)
		return moto_to_errno(r);
	if (r == 0) { // end of directory
		*bytes_read = 0;
		return 0;
	}
	size_t nlen = ent.fname_size;
	if (nlen > 255)
		nlen = 255; // NAME_MAX
	size_t reclen = (offsetof(struct dirent, d_name) + nlen + 1 + 7) & ~size_t(7);
	if (reclen > max_size)
		return EINVAL;
	auto *d = static_cast<struct dirent *>(buffer);
	memset(d, 0, reclen);
	d->d_ino = static_cast<ino_t>(ent.attr.entry_id_lo + 1); // see attr_to_stat
	d->d_off = 0;
	d->d_reclen = static_cast<reclen_t>(reclen);
	d->d_type = ent.attr.file_type == MOTO_FILETYPE_DIRECTORY ? DT_DIR : DT_REG;
	memcpy(d->d_name, ent.fname, nlen);
	d->d_name[nlen] = 0;
	*bytes_read = reclen;
	return 0;
}

int Sysdeps<Ftruncate>::operator()(int fd, size_t size) {
	return moto_to_errno(moto_rt_ftruncate(fd, size));
}

int Sysdeps<Fsync>::operator()(int fd) { return moto_to_errno(moto_rt_fsync(fd)); }

int Sysdeps<Access>::operator()(const char *path, int mode) {
	moto_file_attr_t attr;
	int32_t r =
	    moto_rt_stat(reinterpret_cast<const uint8_t *>(path), strlen(path), &attr);
	if (r < 0)
		return moto_to_errno(r);
	if ((mode & R_OK) && !(attr.perm & MOTO_PERM_READ))
		return EACCES;
	if ((mode & W_OK) && !(attr.perm & MOTO_PERM_WRITE))
		return EACCES;
	// X_OK: directories are traversable; nothing else is executable via libc yet.
	if ((mode & X_OK) && attr.file_type != MOTO_FILETYPE_DIRECTORY)
		return EACCES;
	return 0;
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char *path, int mode, int flags) {
	if (dirfd != AT_FDCWD && path[0] != '/')
		return EBADF;
	// AT_EACCESS is a no-op on a single-user OS; AT_SYMLINK_NOFOLLOW likewise.
	(void)flags;
	return sysdep<Access>(path, mode);
}

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	uint64_t ns;
	switch (clock) {
	case CLOCK_MONOTONIC: ns = moto_rt_mono_nanos(); break;
	case CLOCK_REALTIME:  ns = moto_rt_real_nanos(); break;
	default:              return EINVAL;
	}
	*secs = static_cast<time_t>(ns / 1000000000ul);
	*nanos = static_cast<long>(ns % 1000000000ul);
	return 0;
}

// nanosleep(): Motor's sleep is uninterruptible (no signals), so it always
// completes; *secs/*nanos are the remaining time on return -> always 0.
int Sysdeps<Sleep>::operator()(time_t *secs, long *nanos) {
	uint64_t ns = static_cast<uint64_t>(*secs) * 1000000000ul
	              + static_cast<uint64_t>(*nanos);
	moto_rt_sleep_nanos(ns);
	*secs = 0;
	*nanos = 0;
	return 0;
}

int Sysdeps<Isatty>::operator()(int fd) {
	return moto_rt_is_terminal(fd) ? 0 : ENOTTY;
}

int Sysdeps<GetEntropy>::operator()(void *buffer, size_t length) {
	moto_rt_fill_random_bytes(reinterpret_cast<uint8_t *>(buffer), length);
	return 0;
}

// Motor's runtime has no positional read/write; emulate with
// seek + I/O + seek-back. This races if two threads do positional I/O on
// the SAME fd concurrently — acceptable for the common per-fd
// single-threaded pattern (LLVM's file loading included); a real read_at
// in the VDSO is a wishlist item.
int Sysdeps<Pread>::operator()(int fd, void *buf, size_t n, off_t off, ssize_t *bytes_read) {
	int64_t cur = moto_rt_seek(fd, 0, MOTO_SEEK_CUR);
	if (cur < 0)
		return moto_to_errno(cur);
	int64_t r = moto_rt_seek(fd, off, MOTO_SEEK_SET);
	if (r < 0)
		return moto_to_errno(r);
	int64_t nread = moto_rt_read(fd, reinterpret_cast<uint8_t *>(buf), n);
	moto_rt_seek(fd, cur, MOTO_SEEK_SET); // restore even if the read failed
	if (nread < 0)
		return moto_to_errno(nread);
	*bytes_read = nread;
	return 0;
}

int Sysdeps<Pwrite>::operator()(int fd, const void *buf, size_t n, off_t off, ssize_t *bytes_written) {
	int64_t cur = moto_rt_seek(fd, 0, MOTO_SEEK_CUR);
	if (cur < 0)
		return moto_to_errno(cur);
	int64_t r = moto_rt_seek(fd, off, MOTO_SEEK_SET);
	if (r < 0)
		return moto_to_errno(r);
	int64_t nwritten =
	    moto_rt_write(fd, reinterpret_cast<const uint8_t *>(buf), n);
	moto_rt_seek(fd, cur, MOTO_SEEK_SET); // restore even if the write failed
	if (nwritten < 0)
		return moto_to_errno(nwritten);
	*bytes_written = nwritten;
	return 0;
}

// Motor spawns without fork (posix_spawn semantics are native). The shim
// tracks children by pseudo-pid (>= 0x40000000). file_actions/attrs are not
// supported (no fd inheritance control yet); the child inherits stdio + cwd.
int Sysdeps<PosixSpawn>::operator()(pid_t *ret_pid, const char *path, int have_file_actions,
                                    int have_attr, char *const argv[], char *const envp[]) {
	if (have_file_actions || have_attr)
		return ENOSYS;
	int32_t pid;
	int32_t r = moto_rt_spawn(reinterpret_cast<const uint8_t *>(path), strlen(path),
	                          reinterpret_cast<const uint8_t *const *>(argv),
	                          reinterpret_cast<const uint8_t *const *>(envp), &pid);
	if (r < 0)
		return moto_to_errno(r);
	*ret_pid = pid;
	return 0;
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *ru,
                                 pid_t *ret_pid) {
	if (pid <= 0)
		return ECHILD; // no process groups / wait-for-any on Motor
	if (flags)
		return EINVAL; // no WNOHANG/WUNTRACED: waits are always blocking
	if (ru)
		memset(ru, 0, sizeof *ru);
	int32_t exit_status;
	int32_t r = moto_rt_waitpid(pid, &exit_status);
	if (r == -MOTO_E_BAD_HANDLE)
		return ECHILD; // not our child (or already reaped)
	if (r < 0)
		return moto_to_errno(r);
	if (status)
		*status = (exit_status & 0xff) << 8; // WIFEXITED encoding
	*ret_pid = pid;
	return 0;
}

int Sysdeps<Sysconf>::operator()(int num, long *ret) {
	switch (num) {
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF:
		*ret = static_cast<long>(moto_rt_num_cpus());
		return 0;
	default:
		// EINVAL falls through to mlibc's generic per-key defaults.
		return EINVAL;
	}
}

// Motor has no signal alt-stacks and no resource limits (platform
// properties, not gaps). Implement the sysdeps to fail with a quiet ENOSYS
// instead of leaving them missing: sysdep_or_enosys prints a scary
// missing-sysdep warning, and clang's startup calls sigaltstack and
// getrlimit(RLIMIT_STACK) on every native compile.
int Sysdeps<Sigaltstack>::operator()(const stack_t *, stack_t *) {
	return ENOSYS;
}

int Sysdeps<GetRlimit>::operator()(int, struct rlimit *) { return ENOSYS; }

int Sysdeps<SetRlimit>::operator()(int, const struct rlimit *) {
	return ENOSYS;
}

// Report zero usage rather than failing: callers (LLVM's timers among
// them) tend to use the struct without checking the return value.
int Sysdeps<GetRusage>::operator()(int, struct rusage *usage) {
	memset(usage, 0, sizeof *usage);
	return 0;
}

} // namespace mlibc
