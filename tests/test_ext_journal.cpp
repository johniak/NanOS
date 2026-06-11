#include "doctest.h"
#include "RamBlockDevice.h"
#include "ext/BlockCache.h"
#include "ext/Journal.h"
#include <cstring>
#include <cstdlib>

using namespace kernel;

// JBD2 is big-endian on disk.
static void wbe32(unsigned char* p, unsigned o, unsigned v) {
	p[o] = (unsigned char) (v >> 24); p[o + 1] = (unsigned char) (v >> 16);
	p[o + 2] = (unsigned char) (v >> 8); p[o + 3] = (unsigned char) v;
}
static void wbe16(unsigned char* p, unsigned o, unsigned v) {
	p[o] = (unsigned char) (v >> 8); p[o + 1] = (unsigned char) v;
}
static unsigned be32(const unsigned char* p, unsigned o) {
	return ((unsigned) p[o] << 24) | ((unsigned) p[o + 1] << 16)
	     | ((unsigned) p[o + 2] << 8) | (unsigned) p[o + 3];
}

static const unsigned JBD2_MAGIC = 0xC03B3998u;

// Replay a hand-built, committed JBD2 v1 transaction and confirm it copies the journaled block to
// its destination and resets the log. Validates the big-endian descriptor/commit parsing, the
// ring walk, and the apply step end to end.
TEST_CASE("Journal::replay applies a committed transaction and resets the log") {
	const unsigned BS = 1024;
	const unsigned NBLK = 200;
	unsigned char* disk = (unsigned char*) malloc(NBLK * BS);
	memset(disk, 0, NBLK * BS);
	RamBlockDevice dev("j", disk, NBLK * BS);
	BlockCache cache(&dev, 0, BS);

	// The journal occupies disk blocks 100..107 (journal block i -> disk block 100+i).
	unsigned jb[8];
	for (unsigned i = 0; i < 8; i++) jb[i] = 100 + i;
	const unsigned TARGET = 50;

	// Destination currently holds stale bytes that replay must overwrite.
	unsigned char stale[BS]; memset(stale, 0xAA, BS);
	cache.write(TARGET, stale);

	// The content the committed transaction will restore to TARGET.
	unsigned char want[BS];
	for (unsigned i = 0; i < BS; i++) want[i] = (unsigned char) (i * 7 + 3);

	// Journal superblock (journal block 0): v2 superblock, 1 KiB, len 8, first log block 1,
	// sequence 1, start at block 1 (a transaction is present), no incompat features.
	unsigned char sb[BS]; memset(sb, 0, BS);
	wbe32(sb, 0, JBD2_MAGIC);
	wbe32(sb, 4, 4);            // blocktype = SUPERBLOCK_V2
	wbe32(sb, 12, BS);          // s_blocksize
	wbe32(sb, 16, 8);           // s_maxlen
	wbe32(sb, 20, 1);           // s_first
	wbe32(sb, 24, 1);           // s_sequence
	wbe32(sb, 28, 1);           // s_start
	cache.write(jb[0], sb);

	// Descriptor block (journal block 1): one tag for TARGET, LAST_TAG | SAME_UUID (no UUID).
	unsigned char desc[BS]; memset(desc, 0, BS);
	wbe32(desc, 0, JBD2_MAGIC);
	wbe32(desc, 4, 1);          // blocktype = DESCRIPTOR
	wbe32(desc, 8, 1);          // h_sequence = 1
	wbe32(desc, 12, TARGET);    // t_blocknr
	wbe16(desc, 12 + 6, 8 | 2); // t_flags = LAST_TAG | SAME_UUID
	cache.write(jb[1], desc);

	// Data block (journal block 2): the journaled contents.
	cache.write(jb[2], want);

	// Commit block (journal block 3): same sequence -> transaction is complete.
	unsigned char commit[BS]; memset(commit, 0, BS);
	wbe32(commit, 0, JBD2_MAGIC);
	wbe32(commit, 4, 2);        // blocktype = COMMIT
	wbe32(commit, 8, 1);        // h_sequence = 1
	cache.write(jb[3], commit);

	cache.flush();

	int applied = Journal::replay(&cache, jb, 8);
	CHECK(applied == 1);

	unsigned char got[BS];
	cache.read(TARGET, got);
	for (unsigned i = 0; i < BS; i++) REQUIRE(got[i] == want[i]);

	// The log must be reset: s_start = 0, s_sequence advanced to 2.
	cache.read(jb[0], sb);
	CHECK(be32(sb, 28) == 0);
	CHECK(be32(sb, 24) == 2);

	free(disk);
}

TEST_CASE("Journal::replay is a no-op on a clean log (s_start == 0)") {
	const unsigned BS = 1024;
	unsigned char* disk = (unsigned char*) malloc(120 * BS);
	memset(disk, 0, 120 * BS);
	RamBlockDevice dev("j", disk, 120 * BS);
	BlockCache cache(&dev, 0, BS);
	unsigned jb[8];
	for (unsigned i = 0; i < 8; i++) jb[i] = 100 + i;

	unsigned char sb[BS]; memset(sb, 0, BS);
	wbe32(sb, 0, JBD2_MAGIC);
	wbe32(sb, 12, BS);
	wbe32(sb, 16, 8);
	wbe32(sb, 20, 1);
	wbe32(sb, 24, 5);
	wbe32(sb, 28, 0);           // s_start = 0 -> clean
	cache.write(jb[0], sb);
	cache.flush();

	CHECK(Journal::replay(&cache, jb, 8) == 0);

	// A buffer with no journal magic is declined, not misparsed.
	unsigned jb2[4] = { 10, 11, 12, 13 };
	CHECK(Journal::replay(&cache, jb2, 4) < 0);

	free(disk);
}

TEST_CASE("Journal::replay un-escapes a tagged block, walks a revoke, declines csum_v3") {
	const unsigned BS = 1024;
	unsigned char* disk = (unsigned char*) malloc(200 * BS);
	memset(disk, 0, 200 * BS);
	RamBlockDevice dev("j", disk, 200 * BS);
	BlockCache cache(&dev, 0, BS);
	unsigned jb[16];
	for (unsigned i = 0; i < 16; i++) jb[i] = 100 + i;
	const unsigned TARGET = 60;

	// csum_v3 (incompat 0x10) is declined rather than mis-parsed.
	unsigned char sb[BS]; memset(sb, 0, BS);
	wbe32(sb, 0, JBD2_MAGIC); wbe32(sb, 12, BS); wbe32(sb, 16, 16);
	wbe32(sb, 20, 1); wbe32(sb, 24, 1); wbe32(sb, 28, 1);
	wbe32(sb, 40, 0x10);        // s_feature_incompat = CSUM_V3
	cache.write(jb[0], sb);
	cache.flush();
	CHECK(Journal::replay(&cache, jb, 16) < 0);

	// Now a real transaction: descriptor (revoke block first), an ESCAPE-flagged data block.
	memset(sb, 0, BS);
	wbe32(sb, 0, JBD2_MAGIC); wbe32(sb, 12, BS); wbe32(sb, 16, 16);
	wbe32(sb, 20, 1); wbe32(sb, 24, 1); wbe32(sb, 28, 1);   // start at journal block 1
	cache.write(jb[0], sb);

	// journal block 1: a revoke block (sequence 1) — replay walks past it.
	unsigned char rev[BS]; memset(rev, 0, BS);
	wbe32(rev, 0, JBD2_MAGIC); wbe32(rev, 4, 5); wbe32(rev, 8, 1);   // REVOKE, seq 1
	cache.write(jb[1], rev);

	// journal block 2: descriptor with one ESCAPE|LAST_TAG|SAME_UUID tag for TARGET.
	unsigned char desc[BS]; memset(desc, 0, BS);
	wbe32(desc, 0, JBD2_MAGIC); wbe32(desc, 4, 1); wbe32(desc, 8, 1);
	wbe32(desc, 12, TARGET);
	wbe16(desc, 12 + 6, 1 | 8 | 2);   // ESCAPE | LAST_TAG | SAME_UUID
	cache.write(jb[2], desc);

	// journal block 3: the data (its first word was zeroed by escaping; replay restores the magic).
	unsigned char data[BS]; memset(data, 0, BS);
	for (unsigned i = 4; i < BS; i++) data[i] = (unsigned char) (i & 0xFF);
	cache.write(jb[3], data);

	// journal block 4: commit.
	unsigned char commit[BS]; memset(commit, 0, BS);
	wbe32(commit, 0, JBD2_MAGIC); wbe32(commit, 4, 2); wbe32(commit, 8, 1);
	cache.write(jb[4], commit);
	cache.flush();

	CHECK(Journal::replay(&cache, jb, 16) == 1);
	unsigned char got[BS];
	cache.read(TARGET, got);
	CHECK(be32(got, 0) == JBD2_MAGIC);          // ESCAPE un-done: first word restored to the magic
	for (unsigned i = 4; i < BS; i++) REQUIRE(got[i] == (unsigned char) (i & 0xFF));

	free(disk);
}
