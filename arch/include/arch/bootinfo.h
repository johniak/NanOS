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

// Highest usable physical byte (clamped); a sane fallback if the bootloader
// provided no memory info.
uint32_t bootMemTop();

}
