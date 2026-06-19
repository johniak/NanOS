// tests/usb_mock_hc.h — in-RAM USB host controller for host tests.
#ifndef USB_MOCK_HC_H
#define USB_MOCK_HC_H
#include <arch/usbhc.h>
#include "UsbDescriptors.h"
#include <string.h>
namespace mock {
struct Script {            // one scripted device
    const uint8_t* deviceDesc; unsigned deviceLen;
    const uint8_t* configBlob; unsigned configLen;   // config+interface+endpoint concatenated
    int speed;
};
inline Script*& script() { static Script* s = nullptr; return s; }
inline int enablePort(arch::UsbHc*, int, int* speed){ *speed = script()->speed; return 1; /*slot 1*/ }
inline int configureEndpoint(arch::UsbHc*, int,int,arch::UsbXfer,arch::UsbDir,int){ return 0; }
inline int submit(arch::UsbHc*, arch::UsbTransfer* t){
    // Only GET_DESCRIPTOR control IN is needed for enumeration tests.
    if (t->type==arch::USB_CONTROL && (t->setup.bmRequestType & 0x80)) {
        unsigned dtype = t->setup.wValue >> 8;
        const uint8_t* src=0; unsigned slen=0;
        if (dtype==kernel::USB_DT_DEVICE){ src=script()->deviceDesc; slen=script()->deviceLen; }
        else if (dtype==kernel::USB_DT_CONFIG){ src=script()->configBlob; slen=script()->configLen; }
        unsigned n = t->len < slen ? t->len : slen;
        if (src && t->data) memcpy(t->data, src, n);
        t->result = (int)n;
    } else t->result = 0;
    t->complete = 1; return 0;
}
inline int portCount(arch::UsbHc*){ return 1; }
inline const arch::UsbHcOps OPS = { enablePort, configureEndpoint, submit, portCount };
inline void install(Script* s){ script()=s; arch::usbHcRegister((arch::UsbHc*)1, &OPS); }
}  // namespace mock
#endif
