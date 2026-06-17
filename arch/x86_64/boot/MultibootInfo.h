/*
 * MultibootInfo.h (x86_64) — POD layout of the Multiboot 1 information structure and a
 * memory-map entry, as passed by GRUB. The on-disk Multiboot fields are fixed-width
 * (32-bit), so this is a verbatim mirror of arch/x86/boot/MultibootInfo.h; the long-mode
 * port keeps its own copy because the x86_64 VPATH does not include arch/x86.
 *
 * Reference offsets (Multiboot 1): flags@0, mem_lower@4, mem_upper@8,
 * mmap_length@44, mmap_addr@48.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct MultibootInfo {
	uint32_t flags;            // +0
	uint32_t mem_lower;        // +4   KB below 1MB
	uint32_t mem_upper;        // +8   KB above 1MB
	uint32_t boot_device;      // +12
	uint32_t cmdline;          // +16
	uint32_t mods_count;       // +20
	uint32_t mods_addr;        // +24
	uint32_t syms[4];          // +28..+43
	uint32_t mmap_length;      // +44  bytes
	uint32_t mmap_addr;        // +48  phys addr of the first MmapEntry
	uint32_t drives_length;    // +52
	uint32_t drives_addr;      // +56
	uint32_t config_table;     // +60
	uint32_t boot_loader_name; // +64
	uint32_t apm_table;        // +68
	uint32_t vbe_control_info; // +72
	uint32_t vbe_mode_info;    // +76
	uint16_t vbe_mode;         // +80
	uint16_t vbe_interface_seg;// +82
	uint16_t vbe_interface_off;// +84
	uint16_t vbe_interface_len;// +86
	uint64_t framebuffer_addr; // +88   linear framebuffer physical address
	uint32_t framebuffer_pitch;// +96   bytes per scanline (>= width*bpp/8)
	uint32_t framebuffer_width;// +100
	uint32_t framebuffer_height;//+104
	uint8_t  framebuffer_bpp;  // +108  bits per pixel
	uint8_t  framebuffer_type; // +109  0=indexed, 1=direct RGB, 2=EGA text
	uint8_t  color_info[6];    // +110  RGB field positions (type 1)
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
const uint32_t MB_FLAG_FRAMEBUFFER = 1u << 12;  // framebuffer_* valid (GRUB set a mode)
const uint32_t MMAP_TYPE_AVAILABLE = 1; // usable RAM

// A linear framebuffer GRUB handed us. Mirrors the relevant Multiboot fields.
struct MbFramebuffer {
	uint64_t addr;
	uint32_t pitch, width, height;
	uint8_t  bpp;
};

// Extract the framebuffer GRUB filled (flag bit 12), if it is a linear direct-RGB
// framebuffer (type 1). Returns false otherwise. Pure -> host-testable.
inline bool multibootFramebuffer(const MultibootInfo* mbi, MbFramebuffer* out) {
	if (!mbi || !(mbi->flags & MB_FLAG_FRAMEBUFFER))
		return false;
	if (mbi->framebuffer_type != 1)
		return false;
	out->addr = mbi->framebuffer_addr;
	out->pitch = mbi->framebuffer_pitch;
	out->width = mbi->framebuffer_width;
	out->height = mbi->framebuffer_height;
	out->bpp = mbi->framebuffer_bpp;
	return true;
}

}
