/*
 * arch/random.h — MI/MD contract for hardware entropy.
 *
 * The MI kernel CSPRNG (kernel/Csprng.*) seeds itself from whatever real entropy the
 * platform can give. This contract exposes that: a hardware RNG (x86: RDRAND, when the
 * CPU has it) plus a high-resolution free-running counter the seeder samples for timing
 * jitter (x86: RDTSC). Both are MD; the seeder mixes them with the RTC and never trusts
 * any single source. Included as <arch/random.h> on the kernel + host-test paths.
 */
#pragma once

namespace arch {

// Fill *out with a 32-bit hardware-random word. Returns true on success, false if the CPU
// has no hardware RNG (x86: no RDRAND) — the caller must then fall back to jitter/RTC, never
// to a fixed value. The implementation retries a bounded number of times on RDRAND failure.
bool archHwRandom(unsigned* out);

// A monotonically increasing, high-resolution counter (x86: the low 32 bits of RDTSC). Sampled
// repeatedly by the seeder to harvest timing jitter — the small, unpredictable variations in how
// long each iteration takes. Not random on its own; entropy comes from differences across samples.
unsigned archEntropyTick();

}
