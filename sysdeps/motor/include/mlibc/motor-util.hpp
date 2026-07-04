#pragma once

// Shared Motor-port helpers (used by generic/sysdeps.cpp and generic/thread.cpp).

#include <abi-bits/errno.h>
#include <stdint.h>

namespace mlibc {

// moto ErrorCode (moto-rt/src/error.rs) -> Linux errno. Keep in sync.
inline int moto_to_errno(int64_t e) {
	if (e >= 0)
		return 0;
	switch (-e) {
	case 3:  return EAGAIN;     // NotReady
	case 4:  return ENOSYS;     // NotImplemented
	case 7:  return EINVAL;     // InvalidArgument
	case 8:  return ENOMEM;     // OutOfMemory
	case 9:  return EPERM;      // NotAllowed
	case 10: return ENOENT;     // NotFound
	case 12: return ETIMEDOUT;  // TimedOut
	case 13: return EEXIST;     // AlreadyInUse
	case 14: return EIO;        // UnexpectedEof
	case 15: return EINVAL;     // InvalidFilename
	case 16: return ENOTDIR;    // NotADirectory
	case 17: return EBADF;      // BadHandle
	case 18: return EFBIG;      // FileTooLarge
	case 19: return ENOTCONN;   // NotConnected
	case 20: return ENOSPC;     // StorageFull
	case 21: return EIO;        // InvalidData
	default: return EIO;
	}
}

} // namespace mlibc
