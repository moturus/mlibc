#pragma once

struct stat;

// Pseudo-socket hooks used by generic/sysdeps.cpp (Read/Write/Close) to route
// BSD-socket fds created by socket() to their materialized Motor fds.
// Implementation + design: generic/socket.cpp, docs/porting-libc-appendix-g.md.

namespace mlibc {

constexpr int MOTOR_PSEUDO_FD_BASE = 0x40000000;

// Resolve an application fd for I/O: pseudo-socket fds map to their real fd
// (auto-binding unbound UDP sockets); everything else passes through.
// Returns the real fd, or -errno.
int motor_sock_realfd(int fd);

// close() hook: returns -1 if fd is not a pseudo-socket (caller closes the fd
// normally); otherwise closes the real fd (if any), frees the slot, and
// returns 0 or a positive errno.
int motor_sock_close(int fd);

// fstat() hook: returns -1 if fd is not a pseudo-socket; otherwise fills a
// socket stat without materializing the socket and returns 0 or a positive
// errno.
int motor_sock_fstat(int fd, struct stat *result);

// poll() hook: resolve an application fd for readiness registration. Like
// motor_sock_realfd (auto-binds unbound UDP), but also arms a listening
// socket's async-accept machinery — listener readiness events only fire when
// accept requests are posted (appendix H.4.4). Returns the real fd, -EBADF
// for a bad fd, -EAGAIN for a pseudo-socket with no Motor-side object yet
// (a fresh TCP socket: registerable with nothing, i.e. never ready), or
// another -errno on arming failure.
int motor_sock_pollfd(int fd);

} // namespace mlibc
