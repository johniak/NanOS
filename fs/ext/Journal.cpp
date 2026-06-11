#include "ext/Journal.h"
#include "string.h"

namespace kernel {

// JBD2 on-disk constants (all multi-byte fields are big-endian).
enum {
	JBD2_MAGIC          = 0xC03B3998u,
	JBD2_DESCRIPTOR     = 1,
	JBD2_COMMIT         = 2,
	JBD2_SUPERBLOCK_V1  = 3,
	JBD2_SUPERBLOCK_V2  = 4,
	JBD2_REVOKE         = 5,
	// block-tag flags
	JBD2_FLAG_ESCAPE    = 1,
	JBD2_FLAG_SAME_UUID = 2,
	JBD2_FLAG_DELETED   = 4,
	JBD2_FLAG_LAST_TAG  = 8,
	// incompat features we recognise (others -> decline)
	JBD2_INCOMPAT_REVOKE        = 0x1,
	JBD2_INCOMPAT_64BIT         = 0x2,
	JBD2_INCOMPAT_ASYNC_COMMIT  = 0x4,
	JBD2_INCOMPAT_CSUM_V2       = 0x8,
	JBD2_INCOMPAT_CSUM_V3       = 0x10,
};

static unsigned be32(const unsigned char* p, unsigned o) {
	return ((unsigned) p[o] << 24) | ((unsigned) p[o + 1] << 16)
	     | ((unsigned) p[o + 2] << 8) | (unsigned) p[o + 3];
}
static unsigned short be16(const unsigned char* p, unsigned o) {
	return (unsigned short) ((p[o] << 8) | p[o + 1]);
}
static void wbe32(unsigned char* p, unsigned o, unsigned v) {
	p[o] = (unsigned char) (v >> 24); p[o + 1] = (unsigned char) (v >> 16);
	p[o + 2] = (unsigned char) (v >> 8); p[o + 3] = (unsigned char) v;
}

int Journal::replay(BlockCache* cache, const unsigned* jb, unsigned count) {
	unsigned char sb[BlockCache::MAX_BLOCK];
	cache->read(jb[0], sb);
	if (be32(sb, 0) != JBD2_MAGIC)
		return -1;                                  // not a JBD2 journal
	unsigned jbs = be32(sb, 12);                    // journal block size in bytes
	unsigned maxlen = be32(sb, 16);
	unsigned first = be32(sb, 20);
	unsigned seq = be32(sb, 24);                    // sequence of the first transaction to replay
	unsigned start = be32(sb, 28);                  // log block where it begins (0 = clean)
	unsigned incompat = be32(sb, 40);
	if (start == 0)
		return 0;                                   // nothing to recover
	// We parse the base (v1) tag layout, optionally 64-bit block numbers. Decline csum_v3 (its
	// tag layout differs) and async-commit (changes how the tail is found).
	if (incompat & (JBD2_INCOMPAT_CSUM_V3 | JBD2_INCOMPAT_ASYNC_COMMIT))
		return -2;
	bool feat64 = (incompat & JBD2_INCOMPAT_64BIT) != 0;
	bool hasCsum = (incompat & JBD2_INCOMPAT_CSUM_V2) != 0;   // adds a 2-byte tag checksum we skip
	unsigned tagSize = 8u + (feat64 ? 4u : 0u);              // {blocknr,csum/flags}(8) [+ hi(4)]
	if (maxlen == 0 || maxlen > count)
		maxlen = count;

	// Advance one log block, wrapping from the end back to `first` (the log is a ring).
	struct Ring { unsigned first, maxlen; unsigned next(unsigned b) { unsigned n = b + 1; return n >= maxlen ? first : n; } } ring = { first, maxlen };

	unsigned char blk[BlockCache::MAX_BLOCK];
	unsigned char data[BlockCache::MAX_BLOCK];
	int applied = 0;
	unsigned cur = start;

	// Single forward pass: a transaction is its descriptor + data blocks + a matching commit.
	// Targets are buffered and only written once the commit for that sequence is seen.
	struct Pending { unsigned target; unsigned logBlk; unsigned flags; };
	Pending pend[256];
	int npend = 0;

	for (unsigned guard = 0; guard < maxlen + 2; guard++) {
		cache->read(jb[cur], blk);
		if (be32(blk, 0) != JBD2_MAGIC)
			break;                                  // ran off the end of the written log
		unsigned type = be32(blk, 4);
		unsigned hseq = be32(blk, 8);
		if (hseq != seq)
			break;                                  // next transaction not committed yet -> stop
		if (type == JBD2_DESCRIPTOR) {
			unsigned off = 12;
			unsigned logBlk = cur;
			npend = 0;
			while (off + tagSize <= jbs) {
				unsigned target = be32(blk, off);
				unsigned flags = be16(blk, off + 6);
				logBlk = ring.next(logBlk);         // the data block for this tag
				if (npend < 256) { pend[npend].target = target; pend[npend].logBlk = logBlk; pend[npend].flags = flags; npend++; }
				off += tagSize;
				if (!(flags & JBD2_FLAG_SAME_UUID))
					off += 16;                      // a 16-byte UUID follows non-SAME_UUID tags
				if (flags & JBD2_FLAG_LAST_TAG)
					break;
			}
			(void) hasCsum;
			// step cur past the descriptor and its data blocks
			cur = logBlk;
			cur = ring.next(cur);
		} else if (type == JBD2_COMMIT) {
			for (int i = 0; i < npend; i++) {
				cache->read(jb[pend[i].logBlk], data);
				if (pend[i].flags & JBD2_FLAG_ESCAPE)
					wbe32(data, 0, JBD2_MAGIC);     // un-escape: restore the magic the log zeroed
				cache->write(pend[i].target, data);
				applied++;
			}
			npend = 0;
			seq++;                                  // expect the next transaction's sequence
			cur = ring.next(cur);
		} else if (type == JBD2_REVOKE) {
			cur = ring.next(cur);                   // (single-pass replay: revokes within one txn)
		} else {
			break;
		}
	}

	// Reset the journal to empty: next mount sees a clean log.
	wbe32(sb, 28, 0);          // s_start = 0
	wbe32(sb, 24, seq);        // s_sequence = next expected
	cache->write(jb[0], sb);
	cache->flush();
	return applied;
}

int Journal::writeTxn(BlockCache* cache, const unsigned* jb, unsigned count,
                      const unsigned* targets, unsigned n) {
	unsigned char sb[BlockCache::MAX_BLOCK];
	cache->read(jb[0], sb);
	if (be32(sb, 0) != JBD2_MAGIC)
		return -1;
	unsigned incompat = be32(sb, 40);
	if (incompat & (JBD2_INCOMPAT_CSUM_V3 | JBD2_INCOMPAT_ASYNC_COMMIT | JBD2_INCOMPAT_CSUM_V2))
		return -2;                                  // only the plain v1 layout is written here
	bool feat64 = (incompat & JBD2_INCOMPAT_64BIT) != 0;
	unsigned tagSize = 8u + (feat64 ? 4u : 0u);
	unsigned jbs = be32(sb, 12);
	unsigned maxlen = be32(sb, 16);
	unsigned first = be32(sb, 20);
	unsigned seq = be32(sb, 24);
	if (maxlen == 0 || maxlen > count) maxlen = count;
	if (n == 0)
		return -3;
	// Need 1 descriptor + n data + 1 commit blocks, and the descriptor's tags must fit one block.
	if (first + n + 2 > maxlen)
		return -3;
	if (12u + n * (tagSize + 16u) > jbs)             // worst case: a UUID after every tag
		return -3;

	unsigned char desc[BlockCache::MAX_BLOCK];
	unsigned char data[BlockCache::MAX_BLOCK];
	memset(desc, 0, jbs);
	wbe32(desc, 0, JBD2_MAGIC);
	wbe32(desc, 4, JBD2_DESCRIPTOR);
	wbe32(desc, 8, seq);
	unsigned off = 12;
	unsigned log = first;                            // descriptor sits at journal block `first`
	for (unsigned i = 0; i < n; i++) {
		cache->read(targets[i], data);
		unsigned flags = 0;
		if (be32(data, 0) == JBD2_MAGIC) {           // escape: the block starts with our magic
			flags |= JBD2_FLAG_ESCAPE;
			wbe32(data, 0, 0);
		}
		if (i > 0) flags |= JBD2_FLAG_SAME_UUID;     // only the first tag carries a UUID
		if (i == n - 1) flags |= JBD2_FLAG_LAST_TAG;
		wbe32(desc, off, targets[i]);
		desc[off + 4] = 0; desc[off + 5] = 0;        // (no per-tag checksum without csum_v2)
		desc[off + 6] = (unsigned char) (flags >> 8); desc[off + 7] = (unsigned char) flags;
		off += tagSize;
		if (!(flags & JBD2_FLAG_SAME_UUID)) {        // copy the journal UUID after the first tag
			for (unsigned u = 0; u < 16; u++) desc[off + u] = sb[48 + u];
			off += 16;
		}
		log = (log + 1 >= maxlen) ? first : log + 1; // the data block for this tag
		cache->write(jb[log], data);
	}
	cache->write(jb[first], desc);
	log = (log + 1 >= maxlen) ? first : log + 1;     // commit block follows the last data block
	unsigned char commit[BlockCache::MAX_BLOCK];
	memset(commit, 0, jbs);
	wbe32(commit, 0, JBD2_MAGIC);
	wbe32(commit, 4, JBD2_COMMIT);
	wbe32(commit, 8, seq);
	cache->write(jb[log], commit);

	wbe32(sb, 28, first);                            // s_start -> our transaction
	wbe32(sb, 24, seq);                              // s_sequence -> our transaction's id
	cache->write(jb[0], sb);
	return 0;
}

void Journal::resetLog(BlockCache* cache, const unsigned* jb) {
	unsigned char sb[BlockCache::MAX_BLOCK];
	cache->read(jb[0], sb);
	if (be32(sb, 0) != JBD2_MAGIC)
		return;
	unsigned seq = be32(sb, 24);
	wbe32(sb, 28, 0);            // s_start = 0 (empty)
	wbe32(sb, 24, seq + 1);      // next sequence
	cache->write(jb[0], sb);
}

}  // namespace kernel
