/*
 * mousetest — read the mouse evdev device (/dev/input1) and print the Linux input_event
 * stream the mouse.nkext driver produces. Proves the loadable PS/2 mouse driver end-to-end.
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

struct input_event {
	unsigned tv_sec, tv_usec;
	unsigned short type, code;
	int value;
};

int main(void) {
	int fd = open("/dev/input1", O_RDONLY);
	if (fd < 0) {
		printf("mousetest: cannot open /dev/input1 (is mouse.nkext loaded?)\n");
		return 1;
	}
	printf("mousetest: reading /dev/input1 — move the mouse / click\n");
	struct pollfd pf;
	pf.fd = fd;
	pf.events = POLLIN;
	struct input_event e;
	for (;;) {
		poll(&pf, 1, -1);                  // block until an event is ready
		if (read(fd, &e, sizeof e) != (int) sizeof e)
			continue;
		const char* t = e.type == 2 ? "REL" : e.type == 1 ? "KEY" : e.type == 0 ? "SYN" : "?";
		printf("%s code=%u val=%d\n", t, (unsigned) e.code, e.value);
	}
	return 0;
}
