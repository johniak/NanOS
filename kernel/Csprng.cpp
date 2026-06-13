#include "Csprng.h"
#include <arch/random.h>
#include <arch/cpu.h>     // cpuIrqSave/Restore: serialise the shared instance against IRQ-context reads
#include <string.h>

namespace kernel {

// ---- RFC 8439 ChaCha20 core ----------------------------------------------------------------
static inline unsigned rotl32(unsigned v, int c) { return (v << c) | (v >> (32 - c)); }

#define QR(a, b, c, d)                       \
	a += b; d ^= a; d = rotl32(d, 16);       \
	c += d; b ^= c; b = rotl32(b, 12);       \
	a += b; d ^= a; d = rotl32(d, 8);        \
	c += d; b ^= c; b = rotl32(b, 7)

// Little-endian load of a 32-bit word (the on-disk/on-wire byte order ChaCha20 uses; we never
// assume the host is little-endian, so seeds and vectors round-trip identically everywhere).
static inline unsigned ld32(const unsigned char* p) {
	return (unsigned) p[0] | ((unsigned) p[1] << 8) | ((unsigned) p[2] << 16) | ((unsigned) p[3] << 24);
}
static inline void st32(unsigned char* p, unsigned v) {
	p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8);
	p[2] = (unsigned char) (v >> 16); p[3] = (unsigned char) (v >> 24);
}

void chacha20Block(const unsigned key[8], unsigned counter, const unsigned nonce[3],
		unsigned char out[64]) {
	// State: 4 constant words ("expand 32-byte k"), 8 key words, 1 counter, 3 nonce words.
	unsigned s[16] = {
		0x61707865u, 0x3320646eu, 0x79622d32u, 0x6b206574u,
		key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
		counter, nonce[0], nonce[1], nonce[2],
	};
	unsigned x[16];
	for (int i = 0; i < 16; i++) x[i] = s[i];
	for (int i = 0; i < 10; i++) {            // 20 rounds = 10 column+diagonal double-rounds
		QR(x[0], x[4], x[8],  x[12]);
		QR(x[1], x[5], x[9],  x[13]);
		QR(x[2], x[6], x[10], x[14]);
		QR(x[3], x[7], x[11], x[15]);
		QR(x[0], x[5], x[10], x[15]);
		QR(x[1], x[6], x[11], x[12]);
		QR(x[2], x[7], x[8],  x[13]);
		QR(x[3], x[4], x[9],  x[14]);
	}
	for (int i = 0; i < 16; i++)
		st32(out + i * 4, x[i] + s[i]);
}

#undef QR

// ---- Csprng (fast key erasure) -------------------------------------------------------------
Csprng::Csprng() {
	for (int i = 0; i < 8; i++) m_key[i] = 0;
	m_nonce[0] = m_nonce[1] = m_nonce[2] = 0;
	m_counter = 0;
	m_have = 0;
	m_seeded = false;
}

void Csprng::seed(const void* buf, unsigned len) {
	// Mix the entropy into the key and nonce by XOR-folding (cycling over the bytes), so a short
	// seed perturbs the whole key and a long one keeps contributing. XOR-in (not replace) makes
	// reseed strictly additive — entropy can only accumulate, never be thrown away.
	const unsigned char* in = (const unsigned char*) buf;
	unsigned char* k = (unsigned char*) m_key;     // 32 key bytes
	unsigned char* nb = (unsigned char*) m_nonce;  // 12 nonce bytes
	for (unsigned i = 0; i < len; i++) {
		k[i % 32] = (unsigned char) (k[i % 32] ^ in[i]);
		nb[i % 12] = (unsigned char) (nb[i % 12] ^ (in[i] + (unsigned char) i));
	}
	m_seeded = true;
	m_have = 0;          // discard any buffered keystream; the next bytes() runs the new key
	refill();            // whiten: roll the freshly-keyed state forward at once
	m_have = 0;          // and drop that first chunk too, so seed material never leaks directly
}

void Csprng::refill() {
	unsigned char ks[64];
	chacha20Block(m_key, m_counter, m_nonce, ks);
	if (++m_counter == 0) {                 // 32-bit counter wrap -> advance the nonce
		if (++m_nonce[0] == 0)
			if (++m_nonce[1] == 0)
				++m_nonce[2];
	}
	// Key erasure: the first 32 bytes become the new key (the old key is gone -> forward secrecy);
	// the second 32 bytes are the output we hand out.
	for (int i = 0; i < 8; i++)
		m_key[i] = ld32(ks + i * 4);
	memcpy(m_buf, ks + 32, 32);
	m_have = 32;
}

void Csprng::bytes(void* out, unsigned n) {
	unsigned char* o = (unsigned char*) out;
	while (n) {
		if (m_have == 0)
			refill();
		unsigned take = n < m_have ? n : m_have;
		memcpy(o, m_buf + (32 - m_have), take);
		m_have -= take;
		o += take;
		n -= take;
	}
}

// ---- the shared kernel instance ------------------------------------------------------------
namespace {
Csprng g_csprng;   // .bss; trivial ctor runs lazily via the first seed (globals are not constructed)
}

void csprngSeed(const void* buf, unsigned len) {
	unsigned long fl = arch::cpuIrqSave();
	g_csprng.seed(buf, len);
	arch::cpuIrqRestore(fl);
}

void csprngReseed(const void* buf, unsigned len) {
	unsigned long fl = arch::cpuIrqSave();
	g_csprng.reseed(buf, len);
	arch::cpuIrqRestore(fl);
}

void csprngBytes(void* out, unsigned n) {
	unsigned long fl = arch::cpuIrqSave();
	g_csprng.bytes(out, n);
	arch::cpuIrqRestore(fl);
}

void csprngKernelSeed() {
	// Build a seed pool from every source the platform offers; never trust one alone.
	unsigned pool[40];
	int p = 0;
	// 1) Hardware RNG (RDRAND), if the CPU has it: 16 words of real entropy.
	for (int i = 0; i < 16 && p < 40; i++) {
		unsigned hw;
		if (arch::archHwRandom(&hw))
			pool[p++] = hw;
		else
			break;                       // no RDRAND on this CPU; rely on jitter + RTC below
	}
	// 2) RDTSC timing jitter: sample the counter across a varying amount of work. The entropy is
	//    in the low-bit differences between samples (cache/pipeline/interrupt timing), so fold
	//    successive deltas together rather than storing raw timestamps.
	unsigned prev = arch::archEntropyTick();
	unsigned acc = 0x9e3779b9u;
	for (int i = 0; i < 64 && p < 40; i++) {
		unsigned spin = 0;
		for (volatile int j = 0; j < (int) ((prev & 0x3f) + 1); j++)
			spin += (unsigned) j;
		unsigned now = arch::archEntropyTick();
		acc = (acc ^ (now - prev) ^ spin) * 2654435761u + (acc << 7);
		prev = now;
		if ((i & 3) == 3)                // harvest one folded word every 4 samples
			pool[p++] = acc;
	}
	// 3) The wall-clock epoch — coarse, but distinguishes boots that share identical jitter.
	if (p < 40) pool[p++] = arch::rtcEpoch();
	csprngSeed(pool, (unsigned) p * sizeof(unsigned));
}

}
