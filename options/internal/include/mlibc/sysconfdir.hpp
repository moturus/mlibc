#ifndef MLIBC_SYSCONFDIR_HPP
#define MLIBC_SYSCONFDIR_HPP

// Directory prefix for the classic system configuration files that mlibc reads
// at runtime (passwd, group, hosts, resolv.conf, localtime, ...). Defaults to
// the traditional "/etc"; a sysdep/port may override it at build time by
// defining MLIBC_SYSCONFDIR (e.g. Motor OS builds pass "/sys/cfg/libc").
#ifndef MLIBC_SYSCONFDIR
#define MLIBC_SYSCONFDIR "/etc"
#endif

#endif // MLIBC_SYSCONFDIR_HPP
