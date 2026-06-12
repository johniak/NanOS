/*
 * crashtest.c — deliberately triggers a ring-3 CPU fault (writes to an unmapped address) to prove
 * the kernel kills ONLY this process (like SIGSEGV) and the shell/system keeps running, instead of
 * halting the whole machine. Run it from a shell: the prompt should come back.
 */
int main(void)
{
	volatile int *bad = (volatile int *) 0xDEAD0000;   /* unmapped */
	*bad = 0x1234;                                      /* #PF from ring 3 -> process killed */
	return 0;                                           /* never reached */
}
