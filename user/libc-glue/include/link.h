/*
 * link.h — minimal ELF dynamic-linking introspection for ports that call dl_iterate_phdr
 * (Mesa's util/build_id.c). NanOS runs a single .nxe per process with its own module loader
 * (DynLoader), so there is no glibc-style phdr chain to walk; dl_iterate_phdr (in libc-glue)
 * iterates nothing and returns 0. Consumers use it only to locate an ELF build-id note, which
 * is a shader-cache optimization we don't need (cache disabled) — no build-id is harmless.
 */
#ifndef _NANOS_LINK_H
#define _NANOS_LINK_H
#include <elf.h>
#include <stddef.h>

#ifndef ElfW
#define ElfW(type) Elf64_##type
#endif

struct dl_phdr_info {
	Elf64_Addr         dlpi_addr;    /* base address of the object */
	const char        *dlpi_name;    /* object path */
	const Elf64_Phdr  *dlpi_phdr;    /* program headers */
	Elf64_Half         dlpi_phnum;   /* number of program headers */
};

int dl_iterate_phdr(int (*callback)(struct dl_phdr_info *info, size_t size, void *data),
                    void *data);

#endif /* _NANOS_LINK_H */
