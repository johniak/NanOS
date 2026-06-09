/*
 * envtest — exercise the environment API (getenv/setenv/unsetenv/putenv) in QEMU.
 *
 * Verifies the envp/environ chain end to end: TERM is inherited from the kernel, new
 * variables can be added and read back, an INHERITED variable (whose string lives on the
 * initial stack image) can be overwritten safely (the libc copies environ to the heap on
 * first mutation instead of free()ing a stack pointer), no-overwrite is honoured, unsetenv
 * removes, and putenv installs a caller-owned entry. Prints one PASS/FAIL line.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;

static void expect(const char* what, const char* got, const char* want) {
	int ok = (want == NULL) ? (got == NULL) : (got && !strcmp(got, want));
	if (!ok) {
		fails++;
		printf("  FAIL %s: got '%s', want '%s'\n", what, got ? got : "(null)",
		       want ? want : "(null)");
	}
}

static char put_entry[] = "PUT=xyz";   /* putenv requires a persistent string */

int main(void) {
	expect("TERM inherited", getenv("TERM"), "xterm-256color");

	setenv("FOO", "bar", 1);
	expect("setenv new", getenv("FOO"), "bar");

	setenv("FOO", "baz", 1);
	expect("setenv overwrite", getenv("FOO"), "baz");

	setenv("FOO", "no", 0);
	expect("setenv no-overwrite keeps", getenv("FOO"), "baz");

	/* Overwrite an inherited var (its value string is on the initial stack image). */
	setenv("TERM", "vt100", 1);
	expect("overwrite inherited", getenv("TERM"), "vt100");

	unsetenv("FOO");
	expect("unsetenv removes", getenv("FOO"), NULL);

	putenv(put_entry);
	expect("putenv installs", getenv("PUT"), "xyz");

	printf("envtest: %s\n", fails ? "FAIL" : "OK (getenv/setenv/unsetenv/putenv work)");
	return fails ? 1 : 0;
}
