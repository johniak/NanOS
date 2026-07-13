/* shmdualtest — two processes map the same memfd MAP_SHARED and observe each other's writes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== shmdualtest ===\n");
    int fd = memfd_create("shmdual", 0);
    CHECK(fd >= 0, "memfd_create");
    CHECK(ftruncate(fd, 4096) == 0, "ftruncate 4096");

    volatile char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(p != MAP_FAILED, "parent MAP_SHARED");

    pid_t pid = fork();
    if (pid == 0) {
        volatile char *c = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (c == MAP_FAILED) _exit(2);
        while (c[0] != 'A') usleep(10 * 1000);   /* wait for parent write */
        c[1] = 'B';
        _exit(0);
    }
    p[0] = 'A';
    int st; waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child mapped and saw parent write");
    CHECK(p[1] == 'B', "parent sees child write");

    printf(fails ? "shmdualtest: FAIL (%d)\n" : "shmdualtest: PASS\n", fails);
    return fails ? 1 : 0;
}
