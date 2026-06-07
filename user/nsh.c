/*
 * nsh — the NanOS shell. Runs its own readline-style line editor in RAW console
 * mode: arrow keys move the cursor (left/right) and walk an in-RAM command history
 * (up/down), backspace and mid-line insert work like bash. History is RAM-only (not
 * persisted). While a child program runs the console is switched back to cooked.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int fork(void);                                                  /* libc glue */
int execve(const char* path, char* const argv[], char* const envp[]);
int waitpid(int pid, int* status, int options);
void _exit(int code);
int termmode(int raw);                              /* 0 = cooked, 1 = raw    */

#define CAP  256
#define HMAX 32

static char g_hist[HMAX][CAP];
static int  g_hcount;

static void out(const char* s, int n) { write(1, s, n); }
static void outc(char c)              { write(1, &c, 1); }
static void back(int k)               { while (k-- > 0) outc('\b'); }   /* cursor left */

static void hist_add(const char* line) {
	if (line[0] == 0)
		return;
	if (g_hcount > 0 && strcmp(g_hist[g_hcount - 1], line) == 0)
		return;                              /* skip consecutive duplicate */
	if (g_hcount == HMAX) {                  /* full: drop the oldest */
		for (int i = 1; i < HMAX; i++)
			memcpy(g_hist[i - 1], g_hist[i], CAP);
		g_hcount--;
	}
	int l = (int) strlen(line);
	if (l >= CAP) l = CAP - 1;
	memcpy(g_hist[g_hcount], line, l);
	g_hist[g_hcount][l] = 0;
	g_hcount++;
}

/* Read one line with editing. Returns its length, or -1 on EOF. */
static int readline(char* line) {
	int len = 0, cur = 0;
	char saved[CAP];
	int savedlen = 0;
	int browse = g_hcount;                   /* g_hcount = the (new) line being typed */
	line[0] = 0;

	for (;;) {
		char c;
		if (read(0, &c, 1) <= 0)
			return -1;

		if (c == '\n' || c == '\r') {
			outc('\n');
			line[len] = 0;
			return len;
		}
		if (c == 0x08 || c == 0x7f) {        /* backspace: delete left of cursor */
			if (cur > 0) {
				memmove(&line[cur - 1], &line[cur], len - cur);
				len--; cur--;
				outc('\b');
				out(&line[cur], len - cur);  /* redraw tail */
				outc(' ');                   /* erase the trailing leftover */
				back(len - cur + 1);         /* reposition at the cursor */
			}
			continue;
		}
		if (c == 0x1B) {                     /* ESC '[' X — arrow keys */
			char a, b;
			if (read(0, &a, 1) <= 0 || a != '[')
				continue;
			if (read(0, &b, 1) <= 0)
				continue;
			if (b == 'D') {                  /* left */
				if (cur > 0) { outc('\b'); cur--; }
			} else if (b == 'C') {           /* right */
				if (cur < len) { outc(line[cur]); cur++; }
			} else if (b == 'A' || b == 'B') {   /* up / down: history */
				int nb = (b == 'A') ? browse - 1 : browse + 1;
				if (nb < 0 || nb > g_hcount)
					continue;
				if (browse == g_hcount) {    /* stash the in-progress line */
					memcpy(saved, line, len);
					savedlen = len;
				}
				back(cur);                   /* clear the displayed line */
				for (int i = 0; i < len; i++) outc(' ');
				back(len);
				if (nb == g_hcount) {        /* back to the typed line */
					memcpy(line, saved, savedlen);
					len = savedlen;
				} else {
					int l = (int) strlen(g_hist[nb]);
					memcpy(line, g_hist[nb], l);
					len = l;
				}
				line[len] = 0;
				out(line, len);
				cur = len;
				browse = nb;
			}
			continue;
		}
		if (c == 0x04 && len == 0)           /* Ctrl-D on an empty line: EOF */
			return -1;
		if (c >= 0x20 && c < 0x7f) {         /* printable: insert at cursor */
			if (len < CAP - 1) {
				memmove(&line[cur + 1], &line[cur], len - cur);
				line[cur] = c;
				len++;
				outc(c);
				out(&line[cur + 1], len - cur - 1);   /* redraw tail */
				back(len - cur - 1);                  /* reposition after the char */
				cur++;
			}
		}
	}
}

int main(void) {
	char line[CAP];
	termmode(1);                             /* our own line editor */

	for (;;) {
		out("nsh$ ", 5);
		int n = readline(line);
		if (n < 0) {                         /* EOF */
			out("\n", 1);
			termmode(0);
			return 0;
		}
		if (n == 0)
			continue;
		hist_add(line);

		char* argv[16];
		int argc = 0;
		char* p = line;
		while (*p && argc < 15) {
			while (*p == ' ') *p++ = 0;
			if (!*p) break;
			argv[argc++] = p;
			while (*p && *p != ' ') p++;
		}
		argv[argc] = 0;
		if (argc == 0)
			continue;

		if (!strcmp(argv[0], "exit")) {
			termmode(0);
			return argc > 1 ? atoi(argv[1]) : 0;
		}
		if (!strcmp(argv[0], "echo")) {
			for (int i = 1; i < argc; i++) {
				out(argv[i], (int) strlen(argv[i]));
				out((i + 1 < argc) ? " " : "\n", 1);
			}
			if (argc == 1) out("\n", 1);
			continue;
		}

		char* lsargv[3];
		if (!strcmp(argv[0], "ls") && argc == 1) {
			lsargv[0] = (char*) "ls"; lsargv[1] = (char*) "/"; lsargv[2] = 0;
			argv[0] = lsargv[0]; argv[1] = lsargv[1]; argv[2] = 0; argc = 2;
		}

		char path[160];
		snprintf(path, sizeof path, "/disks/main/nanos/bin/%s.nxe", argv[0]);
		termmode(0);                         /* cooked while the child runs */
		int pid = fork();
		if (pid == 0) {                      /* child: become the program */
			char* envp[] = { 0 };
			execve(path, argv, envp);
			printf("nsh: %s: command not found\n", argv[0]);
			_exit(127);                      /* exec failed */
		} else if (pid > 0) {                /* parent: wait for it */
			int st;
			waitpid(pid, &st, 0);
		} else {
			printf("nsh: fork failed\n");
		}
		termmode(1);
	}
}
