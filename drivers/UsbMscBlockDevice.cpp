// drivers/UsbMscBlockDevice.cpp — BlockDevice over USB MSC (read). See UsbMscBlockDevice.h.
#include "UsbMscBlockDevice.h"
namespace kernel {

int UsbMscBlockDevice::readSectors(uint64_t lba, unsigned count, void* buf) {
    unsigned bs = sectorSize();
    uint8_t* p = (uint8_t*) buf;
    // READ(10)'s block count is 16-bit; split larger requests into chunks.
    for (unsigned done = 0; done < count; ) {
        unsigned chunk = count - done;
        if (chunk > 0xFFFF) chunk = 0xFFFF;
        if (usbMscRead10(m_msc, (uint32_t)(lba + done), chunk, p + (uint64_t)done * bs) < 0) return -1;
        done += chunk;
    }
    return 0;   // 0 = success (BlockDevice HAL convention)
}

int UsbMscBlockDevice::writeSectors(uint64_t lba, unsigned count, const void* buf) {
    unsigned bs = sectorSize();
    const uint8_t* p = (const uint8_t*) buf;
    for (unsigned done = 0; done < count; ) {
        unsigned chunk = count - done;
        if (chunk > 0xFFFF) chunk = 0xFFFF;
        if (usbMscWrite10(m_msc, (uint32_t)(lba + done), chunk, p + (uint64_t)done * bs) < 0) return -1;
        done += chunk;
    }
    return 0;
}

}  // namespace kernel
