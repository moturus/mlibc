/* Motor OS crt1. Self-contained: compiled without include paths.
 *
 * The Motor loader passes nothing on the stack (no argc/argv/auxv) and the
 * process must initialize the RT.VDSO vtable before any runtime call, so this
 * crt synthesizes the SysV entry-stack block mlibc's __dlapi_enter parses:
 *   [argc][argv...][NULL][envp...][NULL][auxv (id,value) pairs][AT_NULL].
 * mlibc's static-build rtld derives the program headers from __ehdr_start
 * itself, so the auxv here can stay minimal.
 *
 * The block lives in motor_start's frame, which never returns — mlibc keeps
 * pointers into it for the process lifetime, which is therefore fine. */
typedef unsigned long uptr;

extern void moto_rt_start(void);
extern char **moto_rt_get_args(int *argc);
extern char **moto_rt_get_env(void);
extern void moto_rt_fill_random_bytes(unsigned char *buf, unsigned long len);

extern void __mlibc_entry(uptr *entry_stack, int (*main_fn)(int, char **, char **));
extern int main(int, char **, char **);

#define AT_NULL 0
#define AT_PAGESZ 6
#define AT_SECURE 23
#define AT_RANDOM 25

/* Normally crtbegin.o defines this; we don't link crtbegin, and with no
 * definition lld synthesizes __dso_handle at the image base — address 0 for
 * our static-PIE. Then every compiler-emitted __cxa_atexit(dtor, obj,
 * &__dso_handle) registers with a NULL dso handle, and mlibc's
 * __mlibc_do_finalize runs ALL C++ static destructors in its early
 * "plain atexit" phase — tearing down mlibc's own stdio before destructors
 * that still print (found the hard way at M8; appendix I pitfalls). The
 * conventional self-referential definition gives dtor registrations a real,
 * unique per-executable handle. */
__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;

void motor_start(void) {
	moto_rt_start(); /* fill the VDSO vtable; must be first */

	int argc = 0;
	char **argv = moto_rt_get_args(&argc);
	char **envp = moto_rt_get_env();
	int envc = 0;
	while (envp[envc])
		envc++;

	static unsigned char random_bytes[16];
	moto_rt_fill_random_bytes(random_bytes, sizeof random_bytes);

	uptr block[1 + (argc + 1) + (envc + 1) + 8];
	uptr *p = block;
	*p++ = (uptr)argc;
	for (int i = 0; i < argc; i++)
		*p++ = (uptr)argv[i];
	*p++ = 0;
	for (int i = 0; i < envc; i++)
		*p++ = (uptr)envp[i];
	*p++ = 0;
	*p++ = AT_PAGESZ; *p++ = 4096;
	*p++ = AT_SECURE; *p++ = 0;
	*p++ = AT_RANDOM; *p++ = (uptr)random_bytes;
	*p++ = AT_NULL;   *p++ = 0;

	__mlibc_entry(block, main);
	__builtin_trap(); /* __mlibc_entry calls exit() */
}
