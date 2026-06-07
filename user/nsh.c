/*
 * nsh — the NanOS shell. Reads a line (blocking cooked input from the kernel line
 * discipline), splits it into argv, runs builtins (exit/echo) or spawns
 * /bin/<cmd>.nxe synchronously via the spawn syscall. No pipes/redirection/jobs.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int spawn(const char* path, char* const argv[]);   /* libc glue (int 0x80) */

int main(void) {
	char line[256];

	for (;;) {
		write(1, "nsh$ ", 5);

		int n = read(0, line, sizeof line - 1);
		if (n <= 0) {                       /* EOF (Ctrl-D) -> leave the shell */
			write(1, "\n", 1);
			return 0;
		}
		if (line[n - 1] == '\n')
			n--;
		line[n] = 0;
		if (n == 0)
			continue;

		/* Split into argv on spaces (in place). */
		char* argv[16];
		int argc = 0;
		char* p = line;
		while (*p && argc < 15) {
			while (*p == ' ')
				*p++ = 0;
			if (!*p)
				break;
			argv[argc++] = p;
			while (*p && *p != ' ')
				p++;
		}
		argv[argc] = 0;
		if (argc == 0)
			continue;

		if (!strcmp(argv[0], "exit"))
			return argc > 1 ? atoi(argv[1]) : 0;

		if (!strcmp(argv[0], "echo")) {
			for (int i = 1; i < argc; i++) {
				write(1, argv[i], strlen(argv[i]));
				write(1, (i + 1 < argc) ? " " : "\n", 1);
			}
			if (argc == 1)
				write(1, "\n", 1);
			continue;
		}

		/* Bare "ls" has no cwd to default to; list root (keeps sbase ls verbatim). */
		char* lsargv[3];
		if (!strcmp(argv[0], "ls") && argc == 1) {
			lsargv[0] = (char*) "ls";
			lsargv[1] = (char*) "/";
			lsargv[2] = 0;
			argv[0] = lsargv[0];
			argv[1] = lsargv[1];
			argv[2] = 0;
			argc = 2;
		}

		/* Programs live on the system volume under /disks/main/nanos/bin. */
		char path[160];
		snprintf(path, sizeof path, "/disks/main/nanos/bin/%s.nxe", argv[0]);
		int rc = spawn(path, argv);
		if (rc == -2)                       /* -ENOENT */
			printf("nsh: %s: command not found\n", argv[0]);
	}
}
