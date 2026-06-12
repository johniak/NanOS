/*
 * pty.h — pseudo-terminal helpers (glibc layout). NanOS userland doesn't expose openpty/forkpty
 * yet; these are declared (and backed by ENOSYS stubs in libc) so inetutils' shared sources
 * compile and link. ping never calls them.
 */
#ifndef _PTY_H
#define _PTY_H

struct termios;
struct winsize;

#ifdef __cplusplus
extern "C" {
#endif

int openpty(int* amaster, int* aslave, char* name,
            const struct termios* termp, const struct winsize* winp);
int forkpty(int* amaster, char* name,
            const struct termios* termp, const struct winsize* winp);
int login_tty(int fd);

#ifdef __cplusplus
}
#endif

#endif /* _PTY_H */
