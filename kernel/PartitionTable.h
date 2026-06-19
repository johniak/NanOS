// kernel/PartitionTable.h — find the NanOS root partition's start LBA on an MBR or GPT disk (MI).
#ifndef PARTITION_TABLE_H
#define PARTITION_TABLE_H
#include "BlockDevice.h"
namespace kernel {
// Returns the start LBA of the first partition that looks like an ext filesystem (ext magic 0xEF53
// at the partition's superblock). Handles a protective-MBR/GPT disk and a classic MBR. Falls back to
// LBA 2048 when nothing matches (the historical default). Reads only via the BlockDevice.
unsigned firstFsPartitionLba(BlockDevice* dev);
}
#endif
