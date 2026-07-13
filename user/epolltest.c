/* epolltest — level-triggered readiness over pipe + eventfd, timeout, EPOLL_CTL_DEL,
 * close-while-waiting. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

static void *late_writer(void *arg) {
    int fd = *(int *) arg;
    usleep(200 * 1000);
    uint64_t v = 1; write(fd, &v, 8);
    return 0;
}

int main(void) {
    printf("=== epolltest ===\n");
    int ep = epoll_create1(0);
    CHECK(ep >= 0, "epoll_create1");

    int pfd[2]; pipe(pfd);
    int efd = eventfd(0, 0);
    struct epoll_event ev = { .events = EPOLLIN };
    ev.data.fd = pfd[0]; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, pfd[0], &ev) == 0, "ADD pipe read end");
    ev.data.fd = efd;    CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, "ADD eventfd");

    struct epoll_event out[4];
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "wait timeout=0 with nothing ready -> 0");

    write(pfd[1], "x", 1);
    int n = epoll_wait(ep, out, 4, 1000);
    CHECK(n == 1 && out[0].data.fd == pfd[0], "pipe write wakes with pipe fd");
    n = epoll_wait(ep, out, 4, 0);
    CHECK(n == 1, "level-triggered: still ready until drained");
    char c; read(pfd[0], &c, 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "drained -> not ready");

    pthread_t t; pthread_create(&t, 0, late_writer, &efd);
    n = epoll_wait(ep, out, 4, 2000);           /* blocks ~200ms, then wakes */
    CHECK(n == 1 && out[0].data.fd == efd, "blocking wait woken by other-thread eventfd write");
    pthread_join(t, 0);
    uint64_t v; read(efd, &v, 8);

    CHECK(epoll_ctl(ep, EPOLL_CTL_DEL, pfd[0], 0) == 0, "DEL pipe");
    write(pfd[1], "y", 1);
    CHECK(epoll_wait(ep, out, 4, 0) == 0, "deleted fd no longer reported");

    printf(fails ? "epolltest: FAIL (%d)\n" : "epolltest: PASS\n", fails);
    return fails ? 1 : 0;
}
