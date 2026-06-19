#include "doctest.h"
#include "UsbMsc.h"
#include "UsbMscBlockDevice.h"
#include "usb_mock_hc.h"
using namespace kernel;
TEST_CASE("MSC READ CAPACITY + READ(10) over BOT") {
    mock::installScsiDisk(/*blocks=*/2048, /*blockSize=*/512, /*pattern=*/0xAB);
    UsbMsc msc; REQUIRE(usbMscInit(&msc, /*slot=*/1, /*epIn=*/0x81, /*epOut=*/0x02) == 0);
    uint32_t blocks=0, bsize=0; REQUIRE(usbMscReadCapacity(&msc,&blocks,&bsize)==0);
    CHECK(blocks==2048); CHECK(bsize==512);
    uint8_t buf[512]; REQUIRE(usbMscRead10(&msc, /*lba=*/0, /*count=*/1, buf)==0);
    CHECK(buf[0]==0xAB); CHECK(buf[511]==0xAB);
}
TEST_CASE("UsbMscBlockDevice readSectors maps to READ(10)") {
    mock::installScsiDisk(2048, 512, 0xCD);
    UsbMsc msc; usbMscInit(&msc,1,0x81,0x02);
    UsbMscBlockDevice dev(&msc, "usb0");
    CHECK(dev.sectorSize()==512);
    uint8_t buf[512];
    CHECK(dev.readSectors(0,1,buf)==0);   // 0 = success (BlockDevice HAL convention)
    CHECK(buf[0]==0xCD);
    CHECK(dev.writeSectors(0,1,buf) < 0); // read-only in phase 1
}
