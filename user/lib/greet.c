/*
 * greet.c — a tiny NanOS shared library (greet.ndl). It exports two functions by name;
 * the dynamic loader binds them into any program that imports them (see user/usedll.c).
 * Built with user/dll.ld (preferred base 0x09000000) and `mknx --dll --export ...`; the
 * loader relocates it to its assigned module window, so the absolute addresses below
 * (the returned string pointer) are fixed up at load time.
 */

int nx_greet(void) {
	return 42;
}

const char* nx_greeting(void) {
	return "hello from greet.ndl";
}
