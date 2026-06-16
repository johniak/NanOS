/*
 * sem_named.c — NanOS stubs for the POSIX *named* semaphore API (sem_open/close/unlink).
 *
 * Not part of musl's unnamed-semaphore core (sem_init/wait/post/...). Named semaphores need a
 * filesystem-backed shared-memory namespace (/dev/shm) NanOS does not have, so these are
 * stubbed to fail with ENOSYS rather than vendored. They exist only so a program that
 * references the names links cleanly; the threading tests use unnamed sem_init-based
 * semaphores, which are fully functional via the vendored musl sources.
 */
#include <semaphore.h>
#include <errno.h>

sem_t *sem_open(const char *name, int flags, ...)
{
	(void)name; (void)flags;
	errno = ENOSYS;
	return SEM_FAILED;
}

int sem_close(sem_t *sem)
{
	(void)sem;
	errno = ENOSYS;
	return -1;
}

int sem_unlink(const char *name)
{
	(void)name;
	errno = ENOSYS;
	return -1;
}
