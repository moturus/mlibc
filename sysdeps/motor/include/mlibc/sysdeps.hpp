#pragma once

#include <mlibc/sysdep-signatures.hpp>

namespace mlibc {

struct MotorSysdepTags :
	LibcPanic,
	LibcLog,
	Exit,
	TcbSet,
	FutexTid,
	FutexWait,
	FutexWake,
	AnonAllocate,
	AnonFree,
	VmMap,
	VmUnmap,
	Open,
	Read,
	Write,
	Seek,
	Close,
	ClockGet,
	Isatty,
	Sleep,
	GetEntropy,
	Rmdir,
	Unlinkat,
	Rename,
	Stat,
	GetCwd,
	Chdir,
	Mkdir,
	Mkdirat,
	Openat,
	OpenDir,
	ReadEntries,
	Ftruncate,
	Fsync,
	Access,
	Faccessat,
	PrepareStack,
	Clone,
	ThreadExit,
	Yield,
	Socket,
	Bind,
	Connect,
	Listen,
	Accept,
	Sendto,
	Recvfrom,
	Shutdown,
	Sockname,
	Peername,
	SetSockopt,
	GetSockopt,
	Fcntl,
	Poll,
	Pselect,
	Sigaction,
	Sigprocmask,
	Kill,
	GetPid,
	Sigaltstack,
	GetRlimit,
	SetRlimit,
	GetRusage,
	Pread,
	Pwrite,
	Sysconf,
	PosixSpawn,
	Waitpid,
	ThreadJoin
{};

template <typename Tag>
using Sysdeps = SysdepOf<MotorSysdepTags, Tag>;

struct SysdepTraits {
	static constexpr bool usesRtNetlink = false;
};

} // namespace mlibc
