#include <errno.h>
#include <fcntl.h>
#include <frg/string.hpp>
#include <frg/vector.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/allocator.hpp>
#include <mlibc/motor-util.hpp>
#include <mlibc/spawn-actions.hpp>
#include <moto_rt.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace mlibc {

int Sysdeps<PosixSpawn>::operator()(pid_t *ret_pid, const char *path,
		const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attrs,
		char *const argv[], char *const envp[]) {
	if (attrs && (attrs->__flags & ~POSIX_SPAWN_USEVFORK))
		return ENOSYS;
	frg::string<MemoryAllocator> cwd{getAllocator()};
	frg::vector<int, MemoryAllocator> opened{getAllocator()};
	moto_spawn_options_t options = {
		.program = path, .argv = argv, .envp = envp, .cwd = nullptr,
		.path = getenv("PATH"), .stdio = {0, 1, 2},
		.flags = attrs && attrs->__fn ? MOTO_SPAWN_SEARCH_PATH : 0,
	};
	int result = [&]() -> int {
		auto op = actions ? static_cast<fdop *>(actions->__actions) : nullptr;
		if (op)
			while (op->next) op = op->next;
		for (; op; op = op->prev) {
			if (op->cmd == FDOP_CHDIR) {
				if (op->path[0] == '/')
					cwd = frg::string<MemoryAllocator>{getAllocator()};
				else if (cwd.size())
					cwd += "/";
				cwd += op->path;
				continue;
			}
			if (op->fd < 0) return EBADF;
			if (op->fd > 2) return ENOSYS;
			switch (op->cmd) {
			case FDOP_DUP2:
				if (op->srcfd < 0) return EBADF;
				options.stdio[op->fd] = op->srcfd < 3 ? options.stdio[op->srcfd] : op->srcfd;
				break;
			case FDOP_OPEN: {
				frg::string<MemoryAllocator> name{getAllocator()};
				if (cwd.size() && op->path[0] != '/') {
					name += cwd;
					name += "/";
				}
				name += op->path;
				int fd = open(name.data(), op->oflag, op->mode);
				if (fd < 0) return errno;
				opened.push_back(fd);
				options.stdio[op->fd] = fd;
				break;
			}
			default:
				return ENOSYS;
			}
		}
		options.cwd = cwd.size() ? cwd.data() : nullptr;
		int32_t pid;
		int r = moto_rt_spawn_ex(&options, &pid, nullptr);
		if (r < 0) return moto_to_errno(r);
		*ret_pid = pid;
		return 0;
	}();
	for (int fd : opened)
		close(fd);
	return result;
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags,
		struct rusage *ru, pid_t *ret_pid) {
	if (flags & ~WNOHANG) return EINVAL;
	if (ru) memset(ru, 0, sizeof(*ru));
	int32_t child_status;
	int r = moto_rt_waitpid_ex(pid, flags & WNOHANG ? MOTO_WAIT_NOHANG : 0, &child_status);
	if (r == -MOTO_E_BAD_HANDLE) return ECHILD;
	if (r < 0) return moto_to_errno(r);
	if (status) *status = child_status;
	*ret_pid = r;
	return 0;
}

int motor_popen(const char *command, const char *type, int *fd, pid_t *pid) {
	if (!type || (type[0] != 'r' && type[0] != 'w')
			|| (type[1] && (type[1] != 'e' || type[2])))
		return EINVAL;
	const char *args[] = {"rush", "-c", command, nullptr};
	bool writing = type[0] == 'w';
	moto_spawn_options_t options = {
		.program = "/system/bin/rush", .argv = args, .envp = environ,
		.cwd = nullptr, .path = nullptr,
		.stdio = {writing ? MOTO_STDIO_PIPE : MOTO_STDIO_INHERIT,
			writing ? MOTO_STDIO_INHERIT : MOTO_STDIO_PIPE, MOTO_STDIO_INHERIT},
		.flags = 0,
	};
	int32_t child, fds[3];
	int r = moto_rt_spawn_ex(&options, &child, fds);
	if (r < 0) return moto_to_errno(r);
	*fd = fds[writing ? 0 : 1];
	*pid = child;
	return 0;
}

} // namespace mlibc
