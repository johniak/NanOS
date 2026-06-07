#include "NxFormat.h"

extern char __bss_start[], __bss_end[], __nx_image_size[];
extern void _start(void);

// The .nx header, placed first in the image by nx.ld. Fields are link-time
// constants. No imports: the program traps via int 0x80, so importCount is 0
// (importTable still points in-image at loadBase to satisfy the loader's bounds
// check, but it is never walked).
__attribute__((section(".nxheader"), used))
const NxHeader nx_header = {
	NX_MAGIC,
	1,
	(unsigned) &_start,
	0x400000u,
	(unsigned) __nx_image_size,
	(unsigned) __bss_start,
	(unsigned) __bss_end,
	0x400000u,   /* importTable (unused; importCount == 0) */
	0,           /* importCount */
};
