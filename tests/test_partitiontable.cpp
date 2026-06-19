#include "doctest.h"
#include "PartitionTable.h"
#include "RamBlockDevice.h"
#include <string.h>
using namespace kernel;

static void put32(unsigned char* p, unsigned v){ p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
static void put64(unsigned char* p, unsigned long long v){ for(int i=0;i<8;i++) p[i]=(unsigned char)(v>>(8*i)); }

TEST_CASE("GPT: discovery returns the ext partition's start LBA") {
    static unsigned char buf[64*512];
    memset(buf, 0, sizeof buf);
    // Protective MBR: one entry of type 0xEE at 0x1BE.
    buf[0x1BE + 4] = 0xEE;
    buf[510] = 0x55; buf[511] = 0xAA;
    // GPT header @ LBA1: "EFI PART", PartitionEntryLBA=2 (off 72), NumEntries=4 (off 80), EntrySize=128 (off 84).
    unsigned char* h = buf + 512;
    memcpy(h, "EFI PART", 8);
    put64(h + 72, 2); put32(h + 80, 4); put32(h + 84, 128);
    // Entry array @ LBA2: entry0 = non-ext (start=10), entry1 = ext root (start=34).
    unsigned char* e = buf + 2*512;
    e[0] = 0x11;            put64(e + 32, 10);                 // entry0 type nonzero, StartingLBA=10
    e[128 + 0] = 0x22;      put64(e + 128 + 32, 34);           // entry1 type nonzero, StartingLBA=34
    // ext superblock magic 0xEF53 for the root: byte 1080 of the partition = sector (34+2), offset 56.
    unsigned char* sb = buf + (34 + 2) * 512;
    sb[56] = 0x53; sb[57] = 0xEF;
    // (sector 10+2 left zero -> entry0 is not ext)
    RamBlockDevice dev("gpt", buf, sizeof buf);
    CHECK(firstFsPartitionLba(&dev) == 34);
}

TEST_CASE("MBR: discovery returns the first partition's start LBA") {
    static unsigned char buf[8*512];
    memset(buf, 0, sizeof buf);
    buf[510] = 0x55; buf[511] = 0xAA;
    unsigned char* p = buf + 0x1BE;          // first partition entry
    p[4] = 0x83;                              // type Linux (non-zero, not 0xEE)
    put32(p + 8, 2048);                       // StartingLBA = 2048
    RamBlockDevice dev("mbr", buf, sizeof buf);
    CHECK(firstFsPartitionLba(&dev) == 2048);
}
