// Motor OS pthread bring-up: PrepareStack / Clone / ThreadExit / Yield.
// Design: docs/porting-libc-appendix-f.md (F.2). Threads run on kernel-allocated
// stacks inside the VDSO's thread wrapper; ThreadExit longjmps back to the entry
// frame so the wrapper's on_thread_exiting() (emutls + __cxa_thread_atexit
// destructors) always runs.

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/motor-util.hpp>
#include <mlibc/tcb.hpp>
#include <moto_rt.h>
#include <new>
#include <setjmp.h>
#include <stdint.h>
#include <stdlib.h>

namespace {

struct MotorThreadCookie {
	void *entry;
	void *user_arg;
	Tcb *tcb;
	size_t stack_size;
	// tid handshake: child stores its tid and wakes; Clone waits on this.
	int tid; // 0 = not yet published (futex word)
};

// VDSO TLS key holding the current thread's exit jmp_buf. Lazily created,
// lock-free: losers of the creation race destroy their key.
size_t exit_key_plus1 = 0; // 0 = uninitialized; key = value - 1

size_t exit_key() {
	size_t cur = __atomic_load_n(&exit_key_plus1, __ATOMIC_ACQUIRE);
	if (cur)
		return cur - 1;
	size_t key = moto_rt_tls_create(nullptr);
	size_t expected = 0;
	if (__atomic_compare_exchange_n(&exit_key_plus1, &expected, key + 1, false,
	                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return key;
	moto_rt_tls_destroy(key); // lost the race
	return expected - 1;
}

// The child-side entry, per the managarm reference (x86_64/thread.cpp).
void enter_thread(void *entry, void *user_arg, Tcb *tcb) {
	if (mlibc::sysdep<TcbSet>(tcb))
		__ensure(!"sys_tcb_set() failed");

	// Wait until the parent stores our tid (thread_create does the wake).
	while (!__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED))
		mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);

	__atomic_fetch_or(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);

	tcb->invokeThreadFunc(entry, user_arg);

	mlibc::thread_exit(tcb->returnValue); // noreturn; ends in ThreadExit
}

// Runs inside the VDSO's thread wrapper, on the kernel-allocated stack.
extern "C" void __motor_thread_entry(uint64_t arg) {
	auto *cookie = reinterpret_cast<MotorThreadCookie *>(arg);

	// Publish our tid; unblocks Clone in the parent. The parent's tcb->tid
	// store (which we wait for in enter_thread) is the ack that it is done
	// reading the cookie, so freeing it at the end of this function is safe.
	__atomic_store_n(&cookie->tid, (int)(uint32_t)moto_rt_tid(), __ATOMIC_RELEASE);
	moto_rt_futex_wake((const uint32_t *)&cookie->tid);

	jmp_buf jb;
	moto_rt_tls_set(exit_key(), &jb);
	if (!setjmp(jb))
		enter_thread(cookie->entry, cookie->user_arg, cookie->tcb);
	// Reached via ThreadExit's longjmp.

	moto_rt_tls_set(exit_key(), nullptr);
	free(reinterpret_cast<MotorThreadCookie *>(arg));
	// Returning hands control to the VDSO wrapper: on_thread_exiting() runs
	// the VDSO TLS dtors (emutls storage, __cxa_thread_atexit list), then the
	// thread self-terminates.
}

} // namespace

namespace mlibc {

int Sysdeps<PrepareStack>::operator()(void **stack, void *entry, void *user_arg,
                                      void *tcb, size_t *stack_size,
                                      size_t *guard_size, void **stack_base) {
	if (*stack) {
		// The kernel owns thread stack placement on Motor; a user-supplied
		// stack (pthread_attr_setstack) cannot be honored.
		mlibc::infoLogger()
		    << "mlibc: pthread_attr_setstack() is not supported on Motor"
		    << frg::endlog;
		return EINVAL;
	}
	if (!*stack_size)
		*stack_size = 0x200000; // 2 MiB, mlibc's default
	*guard_size = 0; // the kernel adds its own guard pages

	auto *cookie = static_cast<MotorThreadCookie *>(malloc(sizeof(MotorThreadCookie)));
	if (!cookie)
		return ENOMEM;
	cookie->entry = entry;
	cookie->user_arg = user_arg;
	cookie->tcb = static_cast<Tcb *>(tcb);
	cookie->stack_size = *stack_size;
	cookie->tid = 0;

	*stack = cookie;       // opaque to generic code; consumed by Clone
	*stack_base = nullptr; // kernel-owned; pthread_attr_getstack won't work
	return 0;
}

int Sysdeps<Clone>::operator()(void *tcb, pid_t *pid_out, void *stack) {
	auto *cookie = static_cast<MotorThreadCookie *>(stack);

	int64_t handle = moto_rt_thread_spawn(__motor_thread_entry, cookie->stack_size,
	                                      reinterpret_cast<uint64_t>(cookie));
	if (handle < 0) {
		free(cookie);
		return moto_to_errno(handle);
	}
	// Retain the kernel handle so the ThreadJoin sysdep can wait on it. The
	// handle is signaled only when the thread has fully self-terminated, i.e.
	// AFTER the VDSO exit wrapper runs the C++ thread_local / __cxa_thread_atexit
	// destructors. mlibc's didExit fires earlier (before those dtors), so a
	// didExit-based join races them. The handle is otherwise leaked (freed at
	// process exit); pthread_join never puts it. See F.2.
	__atomic_store_n(&static_cast<Tcb *>(tcb)->sysdepThreadHandle,
	                 static_cast<uint64_t>(handle), __ATOMIC_RELEASE);

	// Wait for the child to publish its tid.
	while (!__atomic_load_n(&cookie->tid, __ATOMIC_ACQUIRE))
		moto_rt_futex_wait((const uint32_t *)&cookie->tid, 0, UINT64_MAX);

	*pid_out = cookie->tid;
	return 0;
}

void Sysdeps<ThreadExit>::operator()() {
	if (void *p = moto_rt_tls_get(exit_key()))
		longjmp(*static_cast<jmp_buf *>(p), 1);

	// Not one of ours — the main thread. POSIX wants pthread_exit(main) to
	// keep the process alive until the last thread exits; unsupported yet.
	sysdep<LibcLog>("mlibc: pthread_exit() on the main thread exits the process");
	moto_rt_proc_exit(0);
}

void Sysdeps<Yield>::operator()() { moto_rt_thread_yield(); }

int Sysdeps<ThreadJoin>::operator()(uint64_t handle) {
	// Blocks until the joinee is fully torn down: moto_rt_thread_join waits on
	// the kernel thread handle, which is signaled at self-termination — after
	// the VDSO exit wrapper has run the thread's C++ TLS destructors. This is
	// the synchronization point pthread_join needs but didExit does not give.
	int32_t r = moto_rt_thread_join(handle);
	return r ? moto_to_errno(r) : 0;
}


// ---------------------------------------------------------------------------
// Lazy TCB for foreign threads.
//
// Threads created by Rust std (moto-rt/VDSO thread spawn), not by
// pthread_create, have UTCB.libc_tcb == 0 (the kernel zeroes it). When such
// a thread calls into mlibc — e.g. rustc's LLVM worker threads taking a
// std::mutex, whose owner bookkeeping reads get_current_tcb()->tid —
// get_current_tcb() (options/internal x86_64 thread.hpp, __motor__ branch)
// calls this hook to materialize a TCB on first use.
//
// The TCB is minimal, mirroring interpreterMain()'s earlyTcb: under emulated
// TLS there is no ELF TLS area to attach. It is allocated straight from the
// VDSO allocator — __rtld_allocateTcb()'s allocator takes locks that read
// get_current_tcb()->tid, which would recurse into this hook.
//
// The TCB is intentionally leaked: it must outlive every mlibc call on the
// thread, including TLS destructors the VDSO runs at thread exit, and
// foreign threads are few (worker pools sized to CPU count).
extern "C" uintptr_t __mlibc_motor_lazy_tcb() {
	void *mem = moto_rt_alloc_zeroed(sizeof(Tcb), alignof(Tcb));
	__ensure(mem);
	auto tcb = new (mem) Tcb{};
	tcb->selfPointer = tcb;
	tcb->tid = (int)(uint32_t)moto_rt_tid();
	tcb->isJoinable = false; // nothing pthread_joins a foreign thread
	__atomic_store_n(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);
	if (mlibc::sysdep<TcbSet>(tcb))
		__ensure(!"sys_tcb_set() failed");
	return reinterpret_cast<uintptr_t>(tcb);
}

} // namespace mlibc
