#pragma once

#include <stdint.h>
#include <mlibc/tcb.hpp>

// Foreign-thread lazy TCB hook (sysdeps/motor/generic/thread.cpp).
extern "C" uintptr_t __mlibc_motor_lazy_tcb();

namespace mlibc {

inline Tcb *get_current_tcb() {
	uintptr_t ptr;
#if defined(__motor__)
	// Motor OS: the kernel owns %fs (it points at the UTCB); the libc TCB
	// pointer lives in UTCB.libc_tcb at fs:0x58 (motor-os shared_mem.rs).
	asm volatile ("movq %%fs:0x58, %0" : "=r"(ptr));
	if (__builtin_expect(!ptr, 0)) {
		// Foreign thread (created by moto-rt, not pthread): materialize a
		// TCB on first use. Defined in sysdeps/motor/generic/thread.cpp.
		ptr = __mlibc_motor_lazy_tcb();
	}
#else
	asm volatile ("movq %%fs:0, %0" : "=r"(ptr));
#endif
	return reinterpret_cast<Tcb *>(ptr);
}

inline uintptr_t get_sp() {
	uintptr_t rsp;
	asm volatile ("mov %%rsp, %0" : "=r"(rsp));
	return rsp;
}

} // namespace mlibc
