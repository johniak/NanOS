/* eventfdtest — counter semantics, nonblocking, poll readiness, fork/dup. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <sys/wait.h>
#include <sys/eventfd.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== eventfdtest ===\n");
    int fd = eventfd(0, EFD_NONBLOCK);
    CHECK(fd >= 0, "eventfd(0, EFD_NONBLOCK) creates");

    uint64_t v = 0;
    CHECK(read(fd, &v, 8) == -1 && errno == EAGAIN, "read on zero counter -> EAGAIN");

    v = 3; CHECK(write(fd, &v, 8) == 8, "write 3");
    v = 4; CHECK(write(fd, &v, 8) == 8, "write 4");

    struct pollfd p = { .fd = fd, .events = POLLIN };
    CHECK(poll(&p, 1, 0) == 1 && (p.revents & POLLIN), "poll sees POLLIN");

    v = 0;
    CHECK(read(fd, &v, 8) == 8 && v == 7, "read drains accumulated 7");
    CHECK(read(fd, &v, 8) == -1 && errno == EAGAIN, "second read -> EAGAIN again");

    int fd2 = dup(fd);
    v = 1; CHECK(write(fd2, &v, 8) == 8, "write via dup");
    v = 0; CHECK(read(fd, &v, 8) == 8 && v == 1, "read via original sees dup write");

    pid_t pid = fork();
    if (pid == 0) { uint64_t c = 5; write(fd, &c, 8); _exit(0); }
    int st; waitpid(pid, &st, 0);
    v = 0;
    CHECK(read(fd, &v, 8) == 8 && v == 5, "child write visible after fork");

    printf(fails ? "eventfdtest: FAIL (%d)\n" : "eventfdtest: PASS\n", fails);
    return fails ? 1 : 0;
}
