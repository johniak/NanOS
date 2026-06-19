// kernel/PartitionTable.cpp — MBR + GPT partition discovery (MI). See PartitionTable.h.
#include "PartitionTable.h"
namespace kernel {

// The freestanding kernel <string.h> has no memcmp; compare the GPT signature byte-by-byte.
static bool isGptSig(const unsigned char* p) {
    static const char sig[8] = { 'E','F','I',' ','P','A','R','T' };
    for (int i = 0; i < 8; i++) if (p[i] != (unsigned char)sig[i]) return false;
    return true;
}

static unsigned long long rd64(const unsigned char* p) {
    unsigned long long v = 0; for (int i = 0; i < 8; i++) v |= (unsigned long long)p[i] << (8 * i); return v;
}
static unsigned rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

// True if the partition starting at `startLba` carries an ext superblock (magic 0xEF53). The ext
// superblock is at byte 1024 of the partition; its s_magic is at superblock offset 56 -> absolute
// byte 1080 = sector (startLba+2), offset 56.
static bool looksExt(BlockDevice* dev, unsigned long long startLba) {
    unsigned char s[512];
    if (dev->readSectors(startLba + 2, 1, s) != 0) return false;
    return s[56] == 0x53 && s[57] == 0xEF;
}

unsigned firstFsPartitionLba(BlockDevice* dev) {
    unsigned char mbr[512];
    if (dev->readSectors(0, 1, mbr) != 0) return 2048;
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) return 2048;

    // GPT? A protective MBR has a single partition entry of type 0xEE.
    bool gpt = false;
    for (int i = 0; i < 4; i++) if (mbr[0x1BE + i * 16 + 4] == 0xEE) gpt = true;

    if (gpt) {
        unsigned char hdr[512];
        if (dev->readSectors(1, 1, hdr) != 0) return 2048;
        if (!isGptSig(hdr)) return 2048;
        unsigned long long entryLba = rd64(hdr + 72);
        unsigned numEntries = rd32(hdr + 80);
        unsigned entrySize  = rd32(hdr + 84);
        if (entrySize < 128 || entrySize > 512 || numEntries > 256) return 2048;
        unsigned char sec[512];
        unsigned perSec = 512 / entrySize;
        for (unsigned i = 0; i < numEntries; i++) {
            if (i % perSec == 0)
                if (dev->readSectors(entryLba + i / perSec, 1, sec) != 0) return 2048;
            const unsigned char* e = sec + (i % perSec) * entrySize;
            // Skip an unused entry (all-zero type GUID).
            bool used = false; for (int b = 0; b < 16; b++) if (e[b]) used = true;
            if (!used) continue;
            unsigned long long start = rd64(e + 32);   // StartingLBA
            if (looksExt(dev, start)) return (unsigned)start;
        }
        return 2048;
    }

    // Classic MBR: first non-empty partition entry.
    for (int i = 0; i < 4; i++) {
        const unsigned char* e = mbr + 0x1BE + i * 16;
        unsigned type = e[4];
        unsigned start = rd32(e + 8);
        if (type != 0 && start != 0) return start;
    }
    return 2048;
}

}  // namespace kernel
