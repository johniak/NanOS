/* memfdtest — proves (1) /tmp is user-writable (1777) and (2) memfd_create returns a usable
 * scratch fd that ftruncate/write/read round-trip. Does NOT test cross-process MAP_SHARED (see
 * shmdualtest / plan 01 Task 1.5 remaining work: file mmaps are currently private copies). */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== memfdtest ===\n");

    /* (1) unprivileged create in /tmp (1777). */
    int t = open("/tmp/.memfdtest-probe", O_CREAT | O_RDWR | O_TRUNC, 0644);
    CHECK(t >= 0, "create a file in /tmp as this user");
    if (t >= 0) {
        CHECK(write(t, "hi", 2) == 2, "write to the /tmp file");
        char b[4] = {0};
        lseek(t, 0, SEEK_SET);
        CHECK(read(t, b, 2) == 2 && b[0] == 'h' && b[1] == 'i', "read it back");
        close(t);
        unlink("/tmp/.memfdtest-probe");
    }

    /* (2) memfd_create -> a usable scratch fd (ftruncate + fd I/O round-trip). */
    int fd = memfd_create("scratch", 0);
    CHECK(fd >= 0, "memfd_create");
    CHECK(ftruncate(fd, 4096) == 0, "ftruncate 4096");
    const char* msg = "NanOS memfd scratch";
    CHECK(write(fd, msg, (unsigned) strlen(msg)) == (int) strlen(msg), "write to memfd");
    char rb[64] = {0};
    lseek(fd, 0, SEEK_SET);
    CHECK(read(fd, rb, (unsigned) strlen(msg)) == (int) strlen(msg) && strcmp(rb, msg) == 0,
          "read memfd contents back");
    close(fd);

    printf(fails ? "memfdtest: FAIL (%d)\n" : "memfdtest: PASS\n", fails);
    return fails ? 1 : 0;
}
