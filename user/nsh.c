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
int kill(int pid, int sig);
void _exit(int code);
int termmode(int raw);                              /* 0 = cooked, 1 = raw    */

#define CAP  256
#define HMAX 32

/* waitpid options + status decoders (our kernel uses the glibc W* encoding). Guarded so
 * they coexist with any picolibc definitions. */
#ifndef WNOHANG
#define WNOHANG       1
#endif
#ifndef WUNTRACED
#define WUNTRACED     2
#endif
#ifndef SIGCONT
#define SIGCONT       18
#endif
#ifndef WIFSTOPPED
#define WIFSTOPPED(s) (((s) & 0xff) == 0x7f)
#endif

static char g_hist[HMAX][CAP];
static int  g_hcount;

/* ---- job control ------------------------------------------------------------ */
#define NJOBS 16
struct job { int used; int pid; int stopped; char cmd[CAP]; };
static struct job g_jobs[NJOBS];

static int job_add(int pid, const char* cmd, int stopped) {
	for (int i = 0; i < NJOBS; i++) {
		if (g_jobs[i].used)
			continue;
		g_jobs[i].used = 1; g_jobs[i].pid = pid; g_jobs[i].stopped = stopped;
		int l = (int) strlen(cmd); if (l >= CAP) l = CAP - 1;
		memcpy(g_jobs[i].cmd, cmd, l); g_jobs[i].cmd[l] = 0;
		return i;
	}
	return -1;
}
static int job_find(int pid) {
	for (int i = 0; i < NJOBS; i++)
		if (g_jobs[i].used && g_jobs[i].pid == pid) return i;
	return -1;
}
static int job_recent_stopped(void) {
	for (int i = NJOBS - 1; i >= 0; i--)
		if (g_jobs[i].used && g_jobs[i].stopped) return i;
	return -1;
}
static int job_recent(void) {                /* most recent job, any state (fg default) */
	for (int i = NJOBS - 1; i >= 0; i--)
		if (g_jobs[i].used) return i;
	return -1;
}

/* Reap finished/stopped background jobs without blocking, announcing each. */
static void reap_bg(void) {
	int st, pid;
	while ((pid = waitpid(-1, &st, WNOHANG | WUNTRACED)) > 0) {
		int idx = job_find(pid);
		if (idx < 0) continue;
		if (WIFSTOPPED(st)) {
			g_jobs[idx].stopped = 1;
			printf("[%d]+  Stopped\t%s\n", idx + 1, g_jobs[idx].cmd);
		} else {
			printf("[%d]+  Done\t%s\n", idx + 1, g_jobs[idx].cmd);
			g_jobs[idx].used = 0;
		}
	}
}

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
		reap_bg();                           /* announce finished/stopped background jobs */
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
		char cmdsave[CAP];
		strcpy(cmdsave, line);               /* remember the command for the job table */

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
		if (!strcmp(argv[0], "jobs")) {
			for (int i = 0; i < NJOBS; i++)
				if (g_jobs[i].used)
					printf("[%d]+  %s\t%s\n", i + 1,
					       g_jobs[i].stopped ? "Stopped" : "Running", g_jobs[i].cmd);
			continue;
		}
		if (!strcmp(argv[0], "fg") || !strcmp(argv[0], "bg")) {
			int bg = (argv[0][0] == 'b');
			/* no-arg default: fg = the most recent job; bg = the most recent stopped one. */
			int idx = argc > 1 ? atoi(argv[1]) - 1 : (bg ? job_recent_stopped() : job_recent());
			if (idx < 0 || idx >= NJOBS || !g_jobs[idx].used) {
				printf("%s: no such job\n", argv[0]);
				continue;
			}
			int jp = g_jobs[idx].pid;
			g_jobs[idx].stopped = 0;
			kill(jp, SIGCONT);                       /* resume the stopped job */
			if (bg) {
				printf("[%d]+  %s &\n", idx + 1, g_jobs[idx].cmd);
				continue;                            /* leave it running in the background */
			}
			printf("%s\n", g_jobs[idx].cmd);         /* fg: bring it to the foreground */
			termmode(0);
			int st;
			waitpid(jp, &st, WUNTRACED);
			if (WIFSTOPPED(st)) {
				g_jobs[idx].stopped = 1;
				printf("\n[%d]+  Stopped\t%s\n", idx + 1, g_jobs[idx].cmd);
			} else {
				g_jobs[idx].used = 0;                /* exited / killed */
			}
			termmode(1);
			continue;
		}

		char* lsargv[3];
		if (!strcmp(argv[0], "ls") && argc == 1) {
			lsargv[0] = (char*) "ls"; lsargv[1] = (char*) "/"; lsargv[2] = 0;
			argv[0] = lsargv[0]; argv[1] = lsargv[1]; argv[2] = 0; argc = 2;
		}

		termmode(0);                         /* cooked while the child runs */
		int pid = fork();
		if (pid == 0) {                      /* child: become the program */
			char* envp[] = { 0 };
			char path[160];
			if (argv[0][0] == '/') {         /* explicit path: run it as given */
				execve(argv[0], argv, envp);
			} else {
				/* System utility: a flat binary in /nanos/bin ... */
				snprintf(path, sizeof path, "/disks/main/nanos/bin/%s.nxe", argv[0]);
				execve(path, argv, envp);    /* returns only if it failed (e.g. ENOENT) */
				/* ... else an app via /bin, a flat directory of symlinks into the app
				 * bundles (a program is registered here to be runnable by name) ... */
				snprintf(path, sizeof path, "/disks/main/bin/%s.nxe", argv[0]);
				execve(path, argv, envp);
				/* ... or, as a fallback, the bundle itself /apps/<name>/<name>.nxe. */
				snprintf(path, sizeof path, "/disks/main/apps/%s/%s.nxe", argv[0], argv[0]);
				execve(path, argv, envp);
			}
			printf("nsh: %s: command not found\n", argv[0]);
			_exit(127);                      /* exec failed */
		} else if (pid > 0) {                /* parent: wait for it */
			int st;
			waitpid(pid, &st, WUNTRACED);
			if (WIFSTOPPED(st)) {            /* Ctrl+Z stopped it: record a job */
				int idx = job_add(pid, cmdsave, 1);
				printf("\n[%d]+  Stopped\t%s\n", idx + 1, cmdsave);
			}
		} else {
			printf("nsh: fork failed\n");
		}
		termmode(1);
	}
}
