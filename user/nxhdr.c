#include "NxFormat.h"
#include "libnanos.h"

extern char __bss_start[], __bss_end[], __nx_image_size[];
extern const NxImport __nx_imports[];
extern void _start(void);

// The .nx header, placed first in the image by nx.ld. Fields are link-time
// constants (symbol addresses / linker-computed values).
__attribute__((section(".nxheader"), used))
const NxHeader nx_header = {
	NX_MAGIC,
	1,
	(unsigned) &_start,
	0x400000u,
	(unsigned) __nx_image_size,
	(unsigned) __bss_start,
	(unsigned) __bss_end,
	(unsigned) __nx_imports,
	NX_NIMPORTS,
};
