/*
 * arch/bootinfo.h — MI/MD contract for boot-time physical memory info.
 *
 * Abstracts how the bootloader describes RAM (x86: the Multiboot memory map;
 * another arch: a device tree). MI code builds the physical frame allocator
 * from the usable ranges and the top-of-RAM, without knowing the source.
 */
#pragma once
#include <stdint.h>

namespace arch {

// Invoked once per usable physical RAM range.
typedef void (*UsableRangeCb)(void* ctx, uint64_t base, uint64_t length);

void bootMemForEachUsable(void* ctx, UsableRangeCb cb);

// Highest usable physical byte (64-bit, capped at the frame-pool capacity); a sane fallback if
// the bootloader provided no memory info.
uint64_t bootMemTop();

// A linear graphics framebuffer the firmware/bootloader set up for us (the
// vesafb/efifb model: the kernel just draws into it). Format read back from the
// bootloader — never hardcoded.
struct BootFramebuffer {
	uint64_t addr;            // physical address of the linear framebuffer
	uint32_t pitch;           // bytes per scanline (stride; may exceed width*bpp/8)
	uint32_t width, height;   // pixels
	uint8_t  bpp;             // bits per pixel (commonly 32 or 24)
};

// The framebuffer the bootloader provided, or nullptr if none (then we stay in
// VGA text mode).
const BootFramebuffer* bootFramebuffer();

}
