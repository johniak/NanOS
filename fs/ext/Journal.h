/*
 * Journal.h — JBD2 journal recovery (replay) for ext3/ext4.
 *
 * A journaled ext filesystem records metadata (and, in data=journal mode, data) changes in a
 * dedicated log inode before writing them to their final locations. If the machine dies between
 * the commit and the checkpoint, the change is durable in the log; replay copies every COMMITTED
 * transaction's blocks to their final homes at the next mount, making a "dirty" journaled image
 * consistent. This is the prerequisite for mounting the boot disk read-write.
 *
 * Self-contained and host-testable: it works purely over BlockCache + the list of the journal
 * inode's data blocks, so it needs no filesystem object. JBD2 is big-endian on disk. We support
 * the unfeatured (v1) journal layout our images use — 8-byte block tags, optional 64-bit block
 * numbers — and decline (return <0) a journal whose incompat features we don't parse.
 */
#ifndef EXT_JOURNAL_H_
#define EXT_JOURNAL_H_

#include "ext/BlockCache.h"

namespace kernel {

class Journal {
public:
	// `journalBlocks[i]` is the absolute fs block of journal block i ([0] = journal superblock);
	// `count` is the journal length in blocks. Applies each committed transaction to its final
	// locations through `cache`, then resets the journal superblock to empty (s_start = 0). A
	// clean journal (s_start already 0) returns 0 and changes nothing. Returns the number of data
	// blocks replayed (>= 0), or < 0 if the journal has no magic / unsupported features.
	static int replay(BlockCache* cache, const unsigned* journalBlocks, unsigned count);
};

}  // namespace kernel

#endif /* EXT_JOURNAL_H_ */
