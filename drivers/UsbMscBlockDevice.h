// drivers/UsbMscBlockDevice.h — a BlockDevice over a USB mass-storage device (MI HAL adapter).
// Maps readSectors -> SCSI READ(10). Read-only in phase 1 (writeSectors -> error); WRITE(10) is the
// follow-on phase. Like RamBlockDevice, this is pure MI and host-tested over the mock controller.
#ifndef USB_MSC_BLOCK_DEVICE_H
#define USB_MSC_BLOCK_DEVICE_H
#include "BlockDevice.h"
#include "UsbMsc.h"
namespace kernel {

class UsbMscBlockDevice : public BlockDevice {
    UsbMsc* m_msc;
    const char* m_name;
public:
    UsbMscBlockDevice(UsbMsc* msc, const char* name) : m_msc(msc), m_name(name) {}
    int readSectors(uint64_t lba, unsigned count, void* buf) override;
    int writeSectors(uint64_t lba, unsigned count, const void* buf) override;
    unsigned sectorSize() override { return m_msc->blockSize ? m_msc->blockSize : 512; }
    const char* name() override { return m_name; }
};

}  // namespace kernel
#endif
