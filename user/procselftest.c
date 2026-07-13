/* procselftest — /proc/self/exe readlink, /proc/self/fd listing, cwd round-trip. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== procselftest ===\n");
    char buf[256]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    CHECK(n > 0, "readlink /proc/self/exe");
    if (n > 0) { buf[n] = 0; CHECK(strstr(buf, "procselftest") != 0, "exe path names this binary"); }

    /* Get a known-open fd without depending on a writable filesystem: dup(0) always succeeds. */
    int fd = dup(0);
    CHECK(fd >= 0, "dup(0) gives a probe fd");
    DIR *d = opendir("/proc/self/fd");
    CHECK(d != 0, "opendir /proc/self/fd");
    int seen = 0;
    if (d) { struct dirent *e; char want[16];
        snprintf(want, sizeof want, "%d", fd);
        while ((e = readdir(d))) if (!strcmp(e->d_name, want)) seen = 1;
        closedir(d); }
    CHECK(seen, "open fd listed in /proc/self/fd");
    close(fd);

    char cwd[256];
    CHECK(getcwd(cwd, sizeof cwd) != 0, "getcwd");
    CHECK(chdir("/tmp") == 0 && getcwd(buf, sizeof buf) && !strcmp(buf, "/tmp"),
          "chdir/getcwd round-trip");

    printf(fails ? "procselftest: FAIL (%d)\n" : "procselftest: PASS\n", fails);
    return fails ? 1 : 0;
}
