/* mmapexectest — the V8 W^X contract: map RW, write code, flip to RX, execute, and verify
 * a write to the now-RX page faults (child dies by signal, parent survives). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int fails;
#define CHECK(cond, name) do { \
    if (cond) printf("  OK  : %s\n", name); \
    else { printf("  FAIL: %s\n", name); fails++; } } while (0)

int main(void) {
    printf("=== mmapexectest ===\n");
    size_t sz = 4096;
    unsigned char *p = mmap(0, sz, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED, "anonymous RW map");

    /* x86_64: mov eax, 42; ret */
    static const unsigned char code[] = { 0xB8, 0x2A, 0x00, 0x00, 0x00, 0xC3 };
    memcpy(p, code, sizeof code);

    CHECK(mprotect(p, sz, PROT_READ | PROT_EXEC) == 0, "mprotect RW -> RX");
    int (*fn)(void) = (int (*)(void)) p;
    CHECK(fn() == 42, "execute flipped page returns 42");

    pid_t pid = fork();
    if (pid == 0) { p[0] = 0x90; _exit(0); }   /* write to RX page: must die */
    int st; waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st), "write to RX page faults the child (W^X holds)");

    CHECK(mprotect(p, sz, PROT_READ | PROT_WRITE) == 0, "flip back RX -> RW");
    p[0] = 0x90;
    CHECK(1, "write after flip-back succeeds");
    munmap(p, sz);

    printf(fails ? "mmapexectest: FAIL (%d)\n" : "mmapexectest: PASS\n", fails);
    return fails ? 1 : 0;
}
