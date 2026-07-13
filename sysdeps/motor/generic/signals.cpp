// Motor OS "signals-lite": Motor has no signal delivery of any kind, but real
// programs install handlers and raise()/abort() must work. So: dispositions
// and the process mask are recorded; raise()/kill(self) delivers the signal
// SYNCHRONOUSLY in the calling thread (conformant for raise(): POSIX requires
// the handler to complete before it returns); asynchronous delivery does not
// exist — nothing is ever pending, EINTR never happens, the mask is pure
// bookkeeping. Design: docs/porting-libc-appendix-h.md (H.5).

#include <abi-bits/errno.h>
#include <bits/ensure.h>
#include <frg/mutex.hpp>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/lock.hpp>
#include <moto_rt.h>
#include <signal.h>
#include <string.h>

namespace {

FutexLock g_sig_lock;
// Zero-initialized: sa_handler == SIG_DFL for every signal.
struct sigaction g_disp[NSIG];
sigset_t g_procmask;

constexpr size_t kSigsetWords = sizeof(sigset_t) / sizeof(unsigned long);

// Signals whose default action on Linux is "ignore".
bool default_ignored(int sig) {
	return sig == SIGCHLD || sig == SIGURG || sig == SIGWINCH || sig == SIGCONT;
}

} // namespace

namespace mlibc {

pid_t Sysdeps<GetPid>::operator()() { return static_cast<pid_t>(moto_rt_getpid()); }

int Sysdeps<Sigaction>::operator()(int sn, const struct sigaction *__restrict act,
                                   struct sigaction *__restrict old) {
	if (sn < 1 || sn >= NSIG)
		return EINVAL;
	if (act && (sn == SIGKILL || sn == SIGSTOP))
		return EINVAL;
	frg::unique_lock guard{g_sig_lock};
	if (old)
		*old = g_disp[sn];
	if (act)
		g_disp[sn] = *act;
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t *__restrict set,
                                     sigset_t *__restrict retrieve) {
	frg::unique_lock guard{g_sig_lock};
	if (retrieve)
		*retrieve = g_procmask;
	if (!set)
		return 0;
	switch (how) {
	case SIG_BLOCK:
		for (size_t i = 0; i < kSigsetWords; i++)
			g_procmask.__sig[i] |= set->__sig[i];
		return 0;
	case SIG_UNBLOCK:
		for (size_t i = 0; i < kSigsetWords; i++)
			g_procmask.__sig[i] &= ~set->__sig[i];
		return 0;
	case SIG_SETMASK:
		g_procmask = *set;
		return 0;
	default:
		return EINVAL;
	}
}

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	pid_t self = static_cast<pid_t>(moto_rt_getpid());
	if (sig == 0) // existence probe
		return pid == self ? 0 : ESRCH;
	if (sig < 1 || sig >= NSIG)
		return EINVAL;
	if (pid != self)
		return ESRCH; // no cross-process signals on Motor

	// Synchronous self-delivery — the raise() path. The process mask is NOT
	// consulted: nothing can be left pending, and abort() unblocks SIGABRT
	// before raising anyway.
	void (*handler)(int);
	{
		frg::unique_lock guard{g_sig_lock};
		handler = g_disp[sig].sa_handler;
		if (handler != SIG_DFL && handler != SIG_IGN
		    && (g_disp[sig].sa_flags & SA_RESETHAND))
			g_disp[sig].sa_handler = SIG_DFL;
	}
	if (handler == SIG_IGN)
		return 0;
	if (handler == SIG_DFL) {
		if (default_ignored(sig))
			return 0;
		// Default action is termination (no job control, no core dumps):
		// exit with the shell-visible 128+sig convention.
		moto_rt_proc_exit(128 + sig);
	}
	handler(sig);
	return 0;
}

} // namespace mlibc
