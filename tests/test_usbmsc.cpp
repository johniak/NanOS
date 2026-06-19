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
}
TEST_CASE("MSC WRITE(10) then READ(10) round-trips bytes") {
    mock::installScsiDisk(2048, 512, 0xAB);
    UsbMsc msc; REQUIRE(usbMscInit(&msc,1,0x81,0x02)==0);
    uint8_t out[512]; for (int i=0;i<512;i++) out[i]=(uint8_t)(0x40+(i&0x1F));
    REQUIRE(usbMscWrite10(&msc, /*lba=*/2, /*count=*/1, out)==0);
    uint8_t in[512]; REQUIRE(usbMscRead10(&msc, 2, 1, in)==0);
    CHECK(in[0]==out[0]); CHECK(in[511]==out[511]);
    uint8_t other[512]; REQUIRE(usbMscRead10(&msc, 0, 1, other)==0);   // unwritten sector
    CHECK(other[0]==0xAB);
}
TEST_CASE("UsbMscBlockDevice writeSectors round-trips via WRITE(10)") {
    mock::installScsiDisk(2048, 512, 0x11);
    UsbMsc msc; usbMscInit(&msc,1,0x81,0x02);
    UsbMscBlockDevice dev(&msc, "usb0");
    uint8_t w[512]; for (int i=0;i<512;i++) w[i]=(uint8_t)(i*3+7);
    CHECK(dev.writeSectors(1,1,w)==0);
    uint8_t r[512]; CHECK(dev.readSectors(1,1,r)==0);
    CHECK(r[0]==w[0]); CHECK(r[255]==w[255]); CHECK(r[511]==w[511]);
}
