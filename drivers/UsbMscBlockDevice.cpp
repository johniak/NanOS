// drivers/UsbMscBlockDevice.cpp — BlockDevice over USB MSC (read). See UsbMscBlockDevice.h.
#include "UsbMscBlockDevice.h"
namespace kernel {

int UsbMscBlockDevice::readSectors(uint64_t lba, unsigned count, void* buf) {
    unsigned bs = sectorSize();
    uint8_t* p = (uint8_t*) buf;
    // ONE READ(10) per sector. A multi-sector bulk-IN transfer returned only the first 512 bytes
    // on real xHCI hardware (Dell) — the rest came back zero, so ext directory blocks (1 KiB+) read
    // back empty and the root parsed as just "."/".." (init.nxe "not found"). QEMU tolerated the
    // larger transfer, hiding it. Same one-command-per-sector rule AtaBlockDevice uses for the
    // analogous ATA multi-sector desync. (Per-sector is slower; a proper multi-TRB bulk path in
    // xhciSubmit is the future optimization.)
    // Retry each sector a few times: a real xHCI bulk-IN can transiently complete short or fail,
    // and bot() now FAILS such a read instead of returning stale data (see UsbMsc.cpp). Re-issuing
    // the whole READ(10) command recovers the sector; without the retry a single flaky read would
    // abort a file (e.g. init reading the tty7 greeter) and fall back to the wrong behaviour.
    for (unsigned i = 0; i < count; i++) {
        int rc = -1;
        for (int tries = 0; tries < 4 && rc < 0; tries++)
            rc = usbMscRead10(m_msc, (uint32_t)(lba + i), 1, p + (uint64_t)i * bs);
        if (rc < 0)
            return -1;
    }
    return 0;   // 0 = success (BlockDevice HAL convention)
}

int UsbMscBlockDevice::writeSectors(uint64_t lba, unsigned count, const void* buf) {
    unsigned bs = sectorSize();
    const uint8_t* p = (const uint8_t*) buf;
    for (unsigned i = 0; i < count; i++) {             // one WRITE(10) per sector — see readSectors
        int rc = -1;                                   // WRITE(10) is idempotent, so a retry is safe
        for (int tries = 0; tries < 4 && rc < 0; tries++)
            rc = usbMscWrite10(m_msc, (uint32_t)(lba + i), 1, p + (uint64_t)i * bs);
        if (rc < 0)
            return -1;
    }
    return 0;
}

}  // namespace kernel
