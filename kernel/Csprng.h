/*
 * Csprng.h — the kernel's cryptographically secure RNG (machine-independent, host-tested).
 *
 * One shared CSPRNG backs everything that needs unpredictable bytes: /dev/random, /dev/urandom,
 * getrandom(2), and (through getentropy) any ported crypto library — TLS session keys, SSH host
 * keys. The previous /dev/random + getentropy used FIXED seeds, so every boot produced the
 * identical stream and any key derived from them was predictable; this module closes that gap.
 *
 * Construction: ChaCha20 in a "fast key erasure" arrangement (the djbsort/arc4random design).
 * Each refill runs one ChaCha20 block; the first 32 output bytes overwrite the key (so past
 * output cannot be recovered from the current state — forward secrecy) and the remaining bytes
 * are handed out. The core block function is plain RFC 8439 ChaCha20, so it is verifiable against
 * the published test vectors. The class is deterministic given its seed (testable); the KERNEL
 * instance is seeded from real entropy (RDRAND + RDTSC jitter + RTC) so it is unpredictable.
 */
#pragma once

namespace kernel {

// RFC 8439 ChaCha20 block function: 20 rounds over the (constants | key | counter | nonce)
// state, producing 64 keystream bytes. Exposed for the known-answer test; `out` gets 64 bytes.
void chacha20Block(const unsigned key[8], unsigned counter, const unsigned nonce[3],
		unsigned char out[64]);

class Csprng {
public:
	Csprng();                                          // zeroed, unseeded (deterministic state)
	void seed(const void* buf, unsigned len);          // (re)key from entropy; whitens immediately
	void reseed(const void* buf, unsigned len) { seed(buf, len); }   // mix more entropy in
	void bytes(void* out, unsigned n);                 // emit n CSPRNG bytes
	bool seeded() const { return m_seeded; }

private:
	void refill();                                     // generate the next 32-byte output chunk

	unsigned m_key[8];          // current ChaCha20 key (rolled forward every refill)
	unsigned m_nonce[3];        // nonce (also stirred by seed())
	unsigned m_counter;         // block counter
	unsigned char m_buf[32];    // pending output bytes (post key-erasure half of a block)
	unsigned m_have;            // bytes left in m_buf
	bool m_seeded;
};

// ---- the shared kernel instance (free functions wrap a file-scope Csprng) ------------------
void csprngSeed(const void* buf, unsigned len);    // (re)seed the shared instance
void csprngReseed(const void* buf, unsigned len);  // mix entropy into the shared instance
void csprngBytes(void* out, unsigned n);           // fill out[0..n) from the shared instance

// Gather real platform entropy (RDRAND if present + RDTSC timing jitter + the RTC epoch) and seed
// the shared instance. Called once early at boot, before anything consumes randomness.
void csprngKernelSeed();

}
