/*
 * login — a minimal "display manager" greeter for the NanOS graphics VT (tty7).
 *
 * The Linux model (gdm/lightdm/sddm), stripped to its essence: init runs this AS ROOT on the
 * graphics console; it authenticates a username + password against /etc/shadow (crypt), then
 * drops privilege to that user (initgroups + setgid + setuid), establishes the login environment,
 * and execs the desktop (nwm) AS THE USER — so the window manager never runs as root, exactly
 * like a real session. On a bad password it just re-prompts; after a few failures it exits and
 * init respawns a fresh greeter (the getty/DM respawn loop).
 *
 * Auth reuses the same account database as the text logins: /etc/passwd + /etc/shadow (symlinks
 * into /disks/main/nanos/config), via getpwnam/getspnam + crypt() — no new credential path.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <crypt.h>
#include <termios.h>
#include <sys/ioctl.h>

#define NWM_PATH  "/disks/main/nanos/bin/nwm.nxe"
#define KDSETMODE 0x4B3A
#define KD_TEXT   0x00
#define MAX_TRIES 3          /* per greeter instance; init respawns us afterwards */

extern char** environ;

/* Read one cooked line from stdin into buf (the kernel line discipline returns the whole line at
 * once, so a single read with a full-size buffer is required — a short read would drop the tail).
 * Strips the trailing newline. Returns the length, or -1 on EOF/error. */
static int read_line(char* buf, int cap) {
	int r = read(0, buf, cap - 1);
	if (r <= 0) return -1;
	if (buf[r - 1] == '\n') r--;
	buf[r] = 0;
	return r;
}

/* Prompt for a password with terminal echo disabled (restored afterwards) — the kernel line
 * discipline honours termios ECHO, so nothing is displayed as it is typed. */
static int read_password(const char* prompt, char* buf, int cap) {
	struct termios old, quiet;
	int have = (tcgetattr(0, &old) == 0);
	if (have) { quiet = old; quiet.c_lflag &= ~ECHO; tcsetattr(0, TCSANOW, &quiet); }
	fputs(prompt, stdout); fflush(stdout);
	int n = read_line(buf, cap);
	if (have) tcsetattr(0, TCSANOW, &old);
	fputs("\n", stdout); fflush(stdout);   /* the suppressed Enter still needs a visible line break */
	return n;
}

/* Verify `pass` against the account's stored hash (shadow first, then the passwd field). Mirrors
 * login(1): an empty hash means no password (accept); a bare "x" with no shadow entry means the
 * account is locked out of password auth (reject). */
static int check_password(struct passwd* pw, const char* pass) {
	const char* hash = pw->pw_passwd;
	struct spwd* sp = getspnam(pw->pw_name);
	if (sp && sp->sp_pwdp && sp->sp_pwdp[0]) hash = sp->sp_pwdp;
	if (!hash || !hash[0]) return 1;
	if (hash[0] == 'x' && !hash[1]) return 0;
	char* got = crypt(pass, hash);
	return got && strcmp(got, hash) == 0;
}

/* True if env entry `e` is "<key>=...". */
static int env_is(const char* e, const char* key) {
	size_t k = strlen(key);
	return strncmp(e, key, k) == 0 && e[k] == '=';
}

/* Build the login environment (like login(1)/nanologin): inherit the boot env, then override
 * HOME/USER/LOGNAME/SHELL/PATH for this user. The inherited env (from init) already carries
 * HOME/SHELL/etc. for whoever launched us (init runs as root -> HOME=/disks/main/root); getenv()
 * returns the FIRST match, so we must DROP those inherited copies, not merely append ours after
 * them — otherwise the desktop's getenv("HOME") would resolve to root's home and per-user files
 * (settings, the open-with store) would never be found. Returns a NULL-terminated array in `out`. */
static void build_env(struct passwd* pw, char** out, int cap,
		char* shellv, char* homev, char* userv, char* logv) {
	const char* home  = (pw->pw_dir   && pw->pw_dir[0])   ? pw->pw_dir   : "/disks/main";
	const char* shell = (pw->pw_shell && pw->pw_shell[0]) ? pw->pw_shell : "/disks/main/nanos/bin/nsh.nxe";
	strcpy(shellv, "SHELL=");   strncat(shellv, shell,      120);
	strcpy(homev,  "HOME=");    strncat(homev,  home,       120);
	strcpy(userv,  "USER=");    strncat(userv,  pw->pw_name, 56);
	strcpy(logv,   "LOGNAME="); strncat(logv,   pw->pw_name, 56);
	int n = 0;
	for (char** e = environ; *e && n < cap - 7; e++) {
		if (env_is(*e, "HOME") || env_is(*e, "USER") || env_is(*e, "LOGNAME") ||
		    env_is(*e, "SHELL") || env_is(*e, "PATH"))
			continue;                  /* drop inherited copies; we set our own below */
		out[n++] = *e;
	}
	out[n++] = shellv;
	out[n++] = homev;
	out[n++] = userv;
	out[n++] = logv;
	out[n++] = (char*) "PATH=/disks/main/nanos/bin:/disks/main/bin";
	out[n] = 0;
}

int main(void) {
	/* We may have been respawned after nwm left tty7 in graphics mode — force it back to text so
	 * the prompt is visible (no-op if it is already text). */
	ioctl(0, KDSETMODE, (void*) KD_TEXT);

	for (int tries = 0; tries < MAX_TRIES; tries++) {
		char user[64], pass[128];
		fputs("\nNanOS graphical login\n\n", stdout);
		fputs("login: ", stdout); fflush(stdout);
		if (read_line(user, sizeof user) < 0) return 0;   /* EOF: let init respawn us */
		if (!user[0]) continue;
		read_password("password: ", pass, sizeof pass);

		struct passwd* pw = getpwnam(user);
		if (!pw || !check_password(pw, pass)) {
			fputs("Login incorrect\n", stdout); fflush(stdout);
			continue;
		}

		/* Authenticated. Become the user BEFORE exec: groups, then gid, then uid (after setuid we
		 * could no longer change gid/groups). nwm then runs unprivileged, as that user. */
		initgroups(pw->pw_name, pw->pw_gid);
		setgid(pw->pw_gid);
		setuid(pw->pw_uid);

		const char* home = (pw->pw_dir && pw->pw_dir[0]) ? pw->pw_dir : "/disks/main";
		static char shellv[160], homev[160], userv[96], logv[96];
		char* newenv[64];
		build_env(pw, newenv, 64, shellv, homev, userv, logv);
		chdir(home);

		char* argv[] = { (char*) "nwm", 0 };
		execve(NWM_PATH, argv, newenv);
		perror("login: exec nwm");        /* exec failed: bail, init respawns the greeter */
		return 127;
	}
	return 0;
}
