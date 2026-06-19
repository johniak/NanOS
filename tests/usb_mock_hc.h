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

// ---- scripted SCSI mass-storage device (for UsbMsc tests) ----
// Answers Bulk-Only Transport: a CBW (bulk-OUT) sets up the response; the following bulk-IN(s)
// deliver the data stage then the CSW. READ(10) data is a constant `pattern`; WRITE(10) is captured.
struct ScsiState {
    uint32_t blocks, blockSize; uint8_t pattern;
    int phase;          // 0 idle, 1 data, 2 status
    uint8_t  cap[8]; int capLen;   // READ CAPACITY reply staged here
    bool readData;      // data stage delivers backing/`pattern` bytes
    bool writeData;     // data stage is an OUT we capture
    uint32_t lba;       // LBA from the current CBW
    uint8_t  back[4096]; bool wrote[8];   // 8-sector backing store (for WRITE(10) round-trips)
    uint32_t tag;
};
inline ScsiState& scsi(){ static ScsiState s; return s; }
inline uint32_t rdBE32(const uint8_t* p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

inline int scsiSubmit(arch::UsbHc*, arch::UsbTransfer* t){
    ScsiState& s = scsi();
    if (t->type==arch::USB_BULK && t->dir==arch::USB_OUT) {
        const uint8_t* p = (const uint8_t*)t->data;
        if (t->len>=31 && p && p[0]==0x55 && p[1]==0x53 && p[2]==0x42 && p[3]==0x43) {  // CBW
            s.tag = (uint32_t)p[4] | ((uint32_t)p[5]<<8) | ((uint32_t)p[6]<<16) | ((uint32_t)p[7]<<24);
            const uint8_t* cdb = p+15; uint8_t op = cdb[0];
            s.readData=false; s.writeData=false;
            if (op==0x25) {                       // READ CAPACITY(10)
                uint32_t lastLba=s.blocks-1;
                s.cap[0]=(uint8_t)(lastLba>>24); s.cap[1]=(uint8_t)(lastLba>>16);
                s.cap[2]=(uint8_t)(lastLba>>8);  s.cap[3]=(uint8_t)lastLba;
                s.cap[4]=(uint8_t)(s.blockSize>>24); s.cap[5]=(uint8_t)(s.blockSize>>16);
                s.cap[6]=(uint8_t)(s.blockSize>>8);  s.cap[7]=(uint8_t)s.blockSize;
                s.capLen=8; s.phase=1;
            } else if (op==0x28) { s.readData=true;  s.lba=rdBE32(cdb+2); s.phase=1; }   // READ(10)
            else if (op==0x2A) { s.writeData=true; s.lba=rdBE32(cdb+2); s.phase=1; }      // WRITE(10)
            else { s.phase=2; }                                     // no data stage
            t->result=(int)t->len; t->complete=1; return 0;
        }
        if (s.phase==1 && s.writeData) {           // WRITE(10) data stage: store into the backing
            const uint8_t* d=(const uint8_t*)t->data;
            uint32_t base=s.lba*s.blockSize;
            for (unsigned off=0; d && off<t->len; off++) { uint32_t gb=base+off; if (gb<sizeof s.back) s.back[gb]=d[off]; }
            for (uint32_t b=s.lba; b*s.blockSize<base+t->len && b<8; b++) s.wrote[b]=true;
            s.phase=2; t->result=(int)t->len; t->complete=1; return 0;
        }
        t->result=(int)t->len; t->complete=1; return 0;
    }
    if (t->type==arch::USB_BULK && t->dir==arch::USB_IN) {
        if (s.phase==1 && !s.writeData) {          // data stage (IN)
            unsigned n;
            if (s.readData) {                       // serve backing where written, else `pattern`
                n=t->len; uint8_t* out=(uint8_t*)t->data; uint32_t base=s.lba*s.blockSize;
                for (unsigned off=0; out && off<n; off++) {
                    uint32_t gb=base+off, sect=gb/s.blockSize;
                    out[off] = (sect<8 && s.wrote[sect] && gb<sizeof s.back) ? s.back[gb] : s.pattern;
                }
            }
            else { n = t->len < (unsigned)s.capLen ? t->len : (unsigned)s.capLen; if (t->data) memcpy(t->data, s.cap, n); }
            s.phase=2; t->result=(int)n; t->complete=1; return 0;
        }
        if (s.phase==2) {                          // CSW
            uint8_t csw[13]={0x55,0x53,0x42,0x53, 0,0,0,0, 0,0,0,0, 0};   // "USBS", residue 0, status PASSED
            csw[4]=(uint8_t)s.tag; csw[5]=(uint8_t)(s.tag>>8); csw[6]=(uint8_t)(s.tag>>16); csw[7]=(uint8_t)(s.tag>>24);
            unsigned n=t->len<13?t->len:13; if (t->data) memcpy(t->data, csw, n);
            s.phase=0; t->result=(int)n; t->complete=1; return 0;
        }
        t->result=0; t->complete=1; return 0;
    }
    t->result=0; t->complete=1; return 0;   // control etc.
}
inline const arch::UsbHcOps SCSI_OPS = { enablePort, configureEndpoint, scsiSubmit, portCount };
inline void installScsiDisk(uint32_t blocks, uint32_t blockSize, uint8_t pattern){
    ScsiState& s = scsi();
    s.blocks=blocks; s.blockSize=blockSize; s.pattern=pattern; s.phase=0; s.tag=0;
    for (int i=0;i<8;i++) s.wrote[i]=false;
    arch::usbHcRegister((arch::UsbHc*)1, &SCSI_OPS);
}
}  // namespace mock
#endif
