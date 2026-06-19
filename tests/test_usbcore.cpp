#include "doctest.h"
#include "UsbCore.h"
#include "usb_mock_hc.h"
using namespace kernel;
TEST_CASE("enumerate parses device + config/interface/endpoint") {
    static const uint8_t dev[18]={18,1, 0x00,0x02, 0,0,0, 64, 0x34,0x12, 0x78,0x56, 0,0, 0,0,0, 1};
    // config(9) + interface(9, class=HID) + endpoint(7, IN interrupt)
    static const uint8_t cfg[]={ 9,2, 25,0, 1, 1,0, 0x80,50,
                                 9,4, 0,0, 1, USB_CLASS_HID,1,1, 0,
                                 7,5, 0x81, 3, 8,0, 10 };
    static mock::Script s{ dev,18, cfg,sizeof cfg, 1 };
    mock::install(&s);
    UsbDevice d;
    REQUIRE(usbEnumeratePort(0, &d) == 0);
    CHECK(d.idVendor == 0x1234);
    CHECK(d.idProduct == 0x5678);
    CHECK(d.numInterfaces == 1);
    CHECK(d.iface[0].bInterfaceClass == USB_CLASS_HID);
    CHECK(d.numEndpoints == 1);
    CHECK(d.endpoint[0].bEndpointAddress == 0x81);
}
