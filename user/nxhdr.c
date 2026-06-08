#include "NxFormat.h"

// Reserve space for the NanOS header at the very start of the image (nx.ld places
// .nxheader first, so it lands at loadBase). The build tool `mknx` fills these bytes in
// from the linked ELF — entry, image extent, bss range, and the reloc/export/import
// tables — so the placeholder only needs to occupy sizeof(NxHeader) PROGBITS bytes. The
// magic keeps the section non-empty (and thus PROGBITS, not NOBITS).
__attribute__((section(".nxheader"), used))
const NxHeader nx_header = { NX_MAGIC };
