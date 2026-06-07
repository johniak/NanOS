/*
 * MultibootInfo.h
 *
 * Plain POD layout of the Multiboot 1 information structure and a memory-map
 * entry, as passed by GRUB in ebx (stashed in the `mbd` global by loader.s).
 * Only the fields the paging bootstrap needs are described precisely; the rest
 * are placeholders so the offsets line up.
 *
 * Reference offsets (Multiboot 1): flags@0, mem_lower@4, mem_upper@8,
 * mmap_length@44, mmap_addr@48.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct MultibootInfo {
	uint32_t flags;         // +0
	uint32_t mem_lower;     // +4   KB below 1MB
	uint32_t mem_upper;     // +8   KB above 1MB
	uint32_t boot_device;   // +12
	uint32_t cmdline;       // +16
	uint32_t mods_count;    // +20
	uint32_t mods_addr;     // +24
	uint32_t syms[4];       // +28..+43
	uint32_t mmap_length;   // +44  bytes
	uint32_t mmap_addr;     // +48  phys addr of the first MmapEntry
} __attribute__((packed));

// One entry of the memory map. `size` excludes itself, so the next entry is at
// (current + size + 4) — the classic Multiboot stride trap.
struct MmapEntry {
	uint32_t size;
	uint64_t base_addr;
	uint64_t length;
	uint32_t type;
} __attribute__((packed));

const uint32_t MB_FLAG_MEM = 1u << 0;   // mem_lower/mem_upper valid
const uint32_t MB_FLAG_MMAP = 1u << 6;  // mmap_length/mmap_addr valid
const uint32_t MMAP_TYPE_AVAILABLE = 1; // usable RAM

}
