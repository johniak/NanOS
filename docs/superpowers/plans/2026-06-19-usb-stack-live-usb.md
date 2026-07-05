# NanOS USB Stack + Live-USB Root — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended)
> or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** A USB stack for the NanOS x86_64 kernel so the whole system can live on a USB pendrive —
xHCI + USB core + Mass-Storage (read) built INTO the kernel as the root-storage driver (mount
`/disks/main` from USB), and USB-HID (keyboard/mouse) as a loadable kext that feeds the existing evdev
input devices NanWM already consumes.

**Architecture:** MI/MD split. The machine-independent USB core (enumeration, descriptors, HID report
parsing, BOT/SCSI) is host-testable via doctest against a **mock host controller**; the machine-
dependent xHCI driver (MMIO, rings, DMA, IRQ) lives in `arch/x86_64/` and implements a new
`<arch/usbhc.h>` contract. The storage path (xHCI+core+MSC) is in-kernel because it drives the root
(same rule that keeps `AtaBlockDevice` in-kernel); USB-HID is not root-critical so it stays a kext.

**Tech Stack:** C++ freestanding (`-ffreestanding -nostdlib`, no STL/exceptions/RTTI; vtables OK),
NASM (`.S`) for any asm, doctest host tests (`make ARCH=x86_64 test`, ≥90% lcov gate), QEMU
`qemu-system-x86_64 -device qemu-xhci -device usb-storage|usb-kbd|usb-mouse` for MD verification.
Spec: `docs/superpowers/specs/2026-06-19-usb-stack-live-usb-design.md`.

**Conventions to follow (read these first):**
- MI code reaches hardware ONLY through `<arch/...>` contracts; `make check-arch` fails on x86 leaks in
  `init/kernel/mm/fs/lib/drivers`. USB core goes in a new top-level `usb/` dir (MI) — add `usb` to the
  `check-arch` MI dirs too.
- No global constructors run — never rely on a non-trivial static ctor; use `new`/zeroed `.bss` + lazy
  init (see `DeviceManager`).
- x86_64 kernel objects build into `bin/k64/` (separate from i686 `bin/`). Build via Docker:
  `make image64` (kernel+image), `make ARCH=x86_64 test` (host tests), `make smoke-x86_64` (MD boot smoke).
- `BlockDevice` HAL: `drivers/BlockDevice.h` (`readSectors/writeSectors/sectorSize/name`). `DeviceManager`
  is the runtime registry. `Vfs::mount(mountpoint, "auto", dev, lba)` probes ext2/ext4.
- Existing input sinks to reuse (DO NOT create new ones): `kernel::kbdFeed(unsigned char sc)` +
  `KeyboardDevice` (`drivers/KeyboardDevice.h`, `/dev/input0`); `MouseDevice` (`kext/mouse/MouseDevice.h`,
  emits `EV_REL/EV_KEY/EV_SYN` via `push(type,code,value,now_us)`), registered with `knx_add_input_dev`.
- Frames for DMA: `kernel::FrameAllocator` (`mm/FrameAllocator.h`, `alloc()` returns a page-aligned phys
  addr == kernel VA, identity-mapped; `free(pa)`). No IOMMU.

---

## File Structure (created/modified — locked decomposition)

**New MI (host-testable, `usb/`):**
- `usb/UsbDescriptors.h` — packed USB descriptor structs (device/config/interface/endpoint) + `enum`s.
- `usb/UsbCore.h` / `usb/UsbCore.cpp` — `UsbDevice` model + enumeration state machine + transfer request
  helpers; drives a host controller through `<arch/usbhc.h>`.
- `usb/UsbHid.h` / `usb/UsbHid.cpp` — HID report-descriptor parser + boot-protocol keyboard & mouse report
  decoders → evdev event emission (via injected sinks, so it's host-testable).
- `usb/UsbMsc.h` / `usb/UsbMsc.cpp` — Bulk-Only Transport state machine + SCSI CDB build/parse
  (`READ CAPACITY(10)`, `READ(10)`; `WRITE(10)` in the follow-on phase).
- `drivers/UsbMscBlockDevice.h` / `.cpp` — `BlockDevice` adapter over a `UsbMsc` (MI; HAL, like
  `RamBlockDevice`).

**New MD contract + driver:**
- `arch/include/arch/usbhc.h` — the MI↔MD host-controller contract (opaque controller + submit/complete).
- `arch/x86_64/drivers/xhci_x86_64.h` / `.cpp` — xHCI driver: PCI discovery, MMIO, rings (cmd/event/
  transfer), ERST, slot/endpoint contexts, port reset, IRQ; implements `<arch/usbhc.h>`.

**New kext:**
- `kext/usbhid/usbhid.cpp` — kext glue: bind to enumerated HID interfaces, run `UsbHid` decoders, feed the
  existing evdev devices. Built to `/nanos/kext/usbhid.nkext`.

**New tests (`tests/`):**
- `tests/usb_mock_hc.h` — mock host controller implementing `<arch/usbhc.h>` for MI tests (scripts device
  descriptor + transfer responses from in-RAM tables).
- `tests/test_usbcore.cpp`, `tests/test_usbhid.cpp`, `tests/test_usbmsc.cpp`.

**Modified:**
- `Makefile` — `MI_SOURCES`/`KOBJ` add `usb/*.o` + `drivers/UsbMscBlockDevice.o`; `arch/x86_64/arch.mk`
  `ARCH_SOURCES` add `xhci_x86_64.o`; `TEST_MODULES`+`COV_PATTERNS` add the usb modules; `X64` kext list +
  `_image64` install `usbhid.nkext`; new `smoke-usb` target; `check-arch` MI dirs add `usb`.
- `kernel/Kernel.cpp` — in `start()`, after console/paging and BEFORE the ATA mount: bring up the in-kernel
  USB storage path, register the `UsbMscBlockDevice`, and make root discovery prefer a USB volume with a
  valid NanOS partition (ATA fallback retained).
- `arch/x86_64/drivers/block_x86_64.cpp` (or `Kernel.cpp` storage wiring) — call the xHCI init.

---

## Phase 1 — `<arch/usbhc.h>` contract + xHCI bring-up to "addressed device"

### Task 1: Define the host-controller contract `<arch/usbhc.h>`

**Files:**
- Create: `arch/include/arch/usbhc.h`

- [ ] **Step 1: Write the contract header.** This is the ONLY surface MI USB code uses to reach a
  controller. Keep it transfer-oriented and controller-agnostic.

```cpp
// arch/include/arch/usbhc.h — MI<->MD USB host-controller contract (included as <arch/usbhc.h>).
#ifndef ARCH_USBHC_H
#define ARCH_USBHC_H
#include <stdint.h>
namespace arch {

enum UsbDir   { USB_OUT = 0, USB_IN = 1 };
enum UsbXfer  { USB_CONTROL = 0, USB_BULK = 2, USB_INT = 3 };
// 8-byte USB SETUP packet (control transfers).
struct UsbSetup { uint8_t bmRequestType, bRequest; uint16_t wValue, wIndex, wLength; } __attribute__((packed));

// One transfer request. `data`/`len` is the data stage (may be 0). For control, `setup` is used.
// `result` is filled by the controller: bytes transferred (>=0) or negative on error/STALL.
struct UsbTransfer {
    int slot;            // device slot id (assigned at address time; 0 before)
    int endpoint;        // endpoint number+dir encoded by the HC (0 = default control EP)
    UsbXfer type;
    UsbDir  dir;
    UsbSetup setup;      // valid for USB_CONTROL
    void*   data;        // buffer (DMA-able; caller owns)
    uint32_t len;
    int     result;      // OUT: >=0 bytes, <0 error
    int     complete;    // OUT: set non-zero by the HC when done (poll model; no callbacks yet)
};

// Opaque controller handle (the MD driver's object). MI never dereferences it.
struct UsbHc;

// Registered by the MD driver at init. MI USB core calls these.
struct UsbHcOps {
    // Reset+enable a freshly-attached port, allocate a device slot, set its address.
    // Returns the slot id (>0) or <0 on failure. `lowSpeed`/`hiSpeed` reported via `speed` out.
    int  (*enablePort)(UsbHc*, int port, int* speed);
    // Configure endpoint(s) for a device after its descriptors are read (max packet, EP types).
    int  (*configureEndpoint)(UsbHc*, int slot, int endpoint, UsbXfer type, UsbDir dir, int maxPacket);
    // Submit a transfer; for the poll model, run it to completion (or until timeout) and fill result.
    int  (*submit)(UsbHc*, UsbTransfer*);
    int  (*portCount)(UsbHc*);
};

// The MD driver registers (hc, ops); MI core fetches the current HC. One HC for now.
void  usbHcRegister(UsbHc* hc, const UsbHcOps* ops);
UsbHc* usbHc();
const UsbHcOps* usbHcOps();

}  // namespace arch
#endif
```

- [ ] **Step 2: Commit.**
```bash
git add arch/include/arch/usbhc.h
git commit -m "feat(usb): <arch/usbhc.h> MI<->MD host-controller contract"
```

### Task 2: USB descriptor structs + parser (MI, host-tested)

**Files:**
- Create: `usb/UsbDescriptors.h`, `usb/UsbCore.h`, `usb/UsbCore.cpp`
- Create: `tests/test_usbcore.cpp`, `tests/usb_mock_hc.h`
- Modify: `Makefile` (MI_SOURCES/KOBJ, TEST_MODULES, COV_PATTERNS, check-arch MI dirs)

- [ ] **Step 1: Write `usb/UsbDescriptors.h`** — packed structs + constants from the USB 2.0 spec ch.9.

```cpp
// usb/UsbDescriptors.h
#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H
#include <stdint.h>
namespace kernel {
enum { USB_DT_DEVICE=1, USB_DT_CONFIG=2, USB_DT_STRING=3, USB_DT_INTERFACE=4, USB_DT_ENDPOINT=5, USB_DT_HID=0x21 };
enum { USB_CLASS_HID=3, USB_CLASS_MASS_STORAGE=8 };
struct UsbDeviceDesc { uint8_t bLength,bDescriptorType; uint16_t bcdUSB; uint8_t bDeviceClass,bDeviceSubClass,
    bDeviceProtocol,bMaxPacketSize0; uint16_t idVendor,idProduct,bcdDevice; uint8_t iManufacturer,iProduct,
    iSerial,bNumConfigurations; } __attribute__((packed));
struct UsbConfigDesc { uint8_t bLength,bDescriptorType; uint16_t wTotalLength; uint8_t bNumInterfaces,
    bConfigurationValue,iConfiguration,bmAttributes,bMaxPower; } __attribute__((packed));
struct UsbInterfaceDesc { uint8_t bLength,bDescriptorType,bInterfaceNumber,bAlternateSetting,bNumEndpoints,
    bInterfaceClass,bInterfaceSubClass,bInterfaceProtocol,iInterface; } __attribute__((packed));
struct UsbEndpointDesc { uint8_t bLength,bDescriptorType,bEndpointAddress,bmAttributes; uint16_t wMaxPacketSize;
    uint8_t bInterval; } __attribute__((packed));
}  // namespace kernel
#endif
```

- [ ] **Step 2: Write the mock host controller `tests/usb_mock_hc.h`** — implements `<arch/usbhc.h>` so MI
  enumeration is testable with zero hardware. It answers control transfers from a scripted descriptor blob.

```cpp
// tests/usb_mock_hc.h — in-RAM USB host controller for host tests.
#include <arch/usbhc.h>
#include <string.h>
namespace mock {
struct Script {            // one scripted device
    const uint8_t* deviceDesc; unsigned deviceLen;
    const uint8_t* configBlob; unsigned configLen;   // config+interface+endpoint concatenated
    int speed;
};
inline Script* g_script = nullptr;
inline int enablePort(arch::UsbHc*, int, int* speed){ *speed = g_script->speed; return 1; /*slot 1*/ }
inline int configureEndpoint(arch::UsbHc*, int,int,arch::UsbXfer,arch::UsbDir,int){ return 0; }
inline int submit(arch::UsbHc*, arch::UsbTransfer* t){
    // Only GET_DESCRIPTOR control IN is needed for enumeration tests.
    if (t->type==arch::USB_CONTROL && (t->setup.bmRequestType & 0x80)) {
        unsigned dtype = t->setup.wValue >> 8;
        const uint8_t* src=0; unsigned slen=0;
        if (dtype==kernel::USB_DT_DEVICE){ src=g_script->deviceDesc; slen=g_script->deviceLen; }
        else if (dtype==kernel::USB_DT_CONFIG){ src=g_script->configBlob; slen=g_script->configLen; }
        unsigned n = t->len < slen ? t->len : slen;
        if (src && t->data) memcpy(t->data, src, n);
        t->result = (int)n;
    } else t->result = 0;
    t->complete = 1; return 0;
}
inline int portCount(arch::UsbHc*){ return 1; }
inline const arch::UsbHcOps OPS = { enablePort, configureEndpoint, submit, portCount };
inline void install(Script* s){ g_script=s; arch::usbHcRegister((arch::UsbHc*)1, &OPS); }
}  // namespace mock
```

- [ ] **Step 3: Write the failing test `tests/test_usbcore.cpp`** (enumeration parses descriptors).

```cpp
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
```

- [ ] **Step 4: Run it; expect FAIL** (`UsbCore.h` / `usbEnumeratePort` undefined).
```bash
make ARCH=x86_64 test 2>&1 | grep -i "test_usbcore\|UsbCore\|error"
```
Expected: compile error — `UsbCore.h` not found.

- [ ] **Step 5: Write `usb/UsbCore.h`** — the device model + enumeration entry point.

```cpp
// usb/UsbCore.h
#ifndef USB_CORE_H
#define USB_CORE_H
#include "UsbDescriptors.h"
#include <arch/usbhc.h>
namespace kernel {
struct UsbDevice {
    int slot, speed;
    uint16_t idVendor, idProduct;
    uint8_t  numInterfaces, numEndpoints, configValue;
    UsbInterfaceDesc iface[8];
    UsbEndpointDesc  endpoint[16];
};
// Control GET_DESCRIPTOR helper over the registered HC.
int usbGetDescriptor(int slot, uint8_t type, uint8_t index, void* buf, uint16_t len);
// Full enumeration of `port`: enablePort -> read device desc -> read config -> SET_CONFIGURATION ->
// populate `out`. Returns 0 on success, <0 on error.
int usbEnumeratePort(int port, UsbDevice* out);
}  // namespace kernel
#endif
```

- [ ] **Step 6: Implement `usb/UsbCore.cpp`** (enumeration + config-blob walk).

```cpp
// usb/UsbCore.cpp
#include "UsbCore.h"
#include <string.h>
namespace kernel {
int usbGetDescriptor(int slot, uint8_t type, uint8_t index, void* buf, uint16_t len) {
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    arch::UsbTransfer t{}; t.slot=slot; t.endpoint=0; t.type=arch::USB_CONTROL; t.dir=arch::USB_IN;
    t.setup = { 0x80, 6 /*GET_DESCRIPTOR*/, (uint16_t)((type<<8)|index), 0, len };
    t.data=buf; t.len=len;
    if (ops->submit(arch::usbHc(), &t) < 0 || t.result < 0) return -1;
    return t.result;
}
static int setConfiguration(int slot, uint8_t cfg) {
    auto ops = arch::usbHcOps();
    arch::UsbTransfer t{}; t.slot=slot; t.type=arch::USB_CONTROL; t.dir=arch::USB_OUT;
    t.setup = { 0x00, 9 /*SET_CONFIGURATION*/, cfg, 0, 0 };
    return (ops->submit(arch::usbHc(), &t) < 0 || t.result < 0) ? -1 : 0;
}
int usbEnumeratePort(int port, UsbDevice* out) {
    auto ops = arch::usbHcOps(); if (!ops) return -1;
    memset(out, 0, sizeof *out);
    out->slot = ops->enablePort(arch::usbHc(), port, &out->speed);
    if (out->slot <= 0) return -1;
    UsbDeviceDesc dd{};
    if (usbGetDescriptor(out->slot, USB_DT_DEVICE, 0, &dd, sizeof dd) < (int)sizeof dd) return -1;
    out->idVendor=dd.idVendor; out->idProduct=dd.idProduct;
    uint8_t blob[256];
    int n = usbGetDescriptor(out->slot, USB_DT_CONFIG, 0, blob, sizeof blob);
    if (n < (int)sizeof(UsbConfigDesc)) return -1;
    UsbConfigDesc* c = (UsbConfigDesc*)blob; out->configValue = c->bConfigurationValue;
    // Walk the concatenated config blob by bLength.
    int off = c->bLength;
    while (off + 2 <= n) {
        uint8_t blen = blob[off], btype = blob[off+1];
        if (blen == 0) break;
        if (btype==USB_DT_INTERFACE && out->numInterfaces<8)
            memcpy(&out->iface[out->numInterfaces++], blob+off, sizeof(UsbInterfaceDesc));
        else if (btype==USB_DT_ENDPOINT && out->numEndpoints<16)
            memcpy(&out->endpoint[out->numEndpoints++], blob+off, sizeof(UsbEndpointDesc));
        off += blen;
    }
    if (setConfiguration(out->slot, out->configValue) < 0) return -1;
    return 0;
}
}  // namespace kernel
```

- [ ] **Step 7: Add the `usbHcRegister/usbHc/usbHcOps` registry** (small, in `usb/UsbCore.cpp` under
  `namespace arch`). Zeroed `.bss` globals (no ctor).
```cpp
namespace arch {
static UsbHc* g_hc = 0; static const UsbHcOps* g_ops = 0;
void usbHcRegister(UsbHc* hc, const UsbHcOps* ops){ g_hc=hc; g_ops=ops; }
UsbHc* usbHc(){ return g_hc; } const UsbHcOps* usbHcOps(){ return g_ops; }
}
```

- [ ] **Step 8: Wire the Makefile** — add to MI build + tests.
  - `MI_SOURCES += UsbCore.o UsbHid.o UsbMsc.o` and `drivers/UsbMscBlockDevice.o` (VPATH already covers
    `drivers`; add `usb` to `VPATH`).
  - `TEST_MODULES += usb/UsbCore.cpp usb/UsbHid.cpp usb/UsbMsc.cpp drivers/UsbMscBlockDevice.cpp`
  - `COV_PATTERNS` add `usb/` + `UsbMscBlockDevice`.
  - `KINCLUDES`/`HINCLUDES` add `-Iusb`.
  - `check-arch` `MI_CHECK_DIRS` add `usb`.

- [ ] **Step 9: Run the test; expect PASS.**
```bash
make ARCH=x86_64 test 2>&1 | grep -iE "test cases|usbcore|fail"
```
Expected: the enumerate test passes; overall suite still green ≥90%.

- [ ] **Step 10: Commit.**
```bash
git add usb/UsbDescriptors.h usb/UsbCore.h usb/UsbCore.cpp tests/test_usbcore.cpp tests/usb_mock_hc.h Makefile
git commit -m "feat(usb): MI USB core — descriptors + enumeration (host-tested via mock HC)"
```

### Task 3: xHCI driver skeleton — PCI discovery + MMIO map + capability/operational regs (MD)

**Files:**
- Create: `arch/x86_64/drivers/xhci_x86_64.h`, `arch/x86_64/drivers/xhci_x86_64.cpp`
- Modify: `arch/x86_64/arch.mk` (`ARCH_SOURCES += xhci_x86_64.o`), storage wiring in `kernel/Kernel.cpp`

> **No host unit test** — this is register-level MD. Verification is QEMU: the driver must find the
> `qemu-xhci` PCI device (8086:xxxx, class 0x0C0330) and read CAPLENGTH/HCSPARAMS. TDD here = a QEMU
> smoke assertion (a `Console` log line) + zero faults.

- [ ] **Step 1: Implement PCI discovery + BAR0 map.** Use the existing `kernel/Pci.cpp` enumeration
  (`arch/x86_64/io/pci_x86_64`). Find class code `0x0C0330` (USB xHCI). Read BAR0 (64-bit MMIO), map it
  (identity-mapped low MMIO via the existing `mmuMapKernelMmio` used by the framebuffer). Read
  `CAPLENGTH` (offset 0) → operational regs base = MMIO + CAPLENGTH; `HCSPARAMS1` → maxPorts/maxSlots.

```cpp
// arch/x86_64/drivers/xhci_x86_64.h
#ifndef XHCI_X86_64_H
#define XHCI_X86_64_H
namespace arch { void xhciInit(); }   // discovers + brings up xHCI; registers via usbHcRegister
#endif
```

- [ ] **Step 2: Add a one-line `Console` probe log** in `xhciInit()`:
  `"xHCI: <maxPorts> ports, <maxSlots> slots @ BAR0=<phys>"` so the QEMU smoke can assert it.

- [ ] **Step 3: Call `arch::xhciInit()` from `Kernel.cpp` storage bring-up** (before ATA mount; guarded so
  the QEMU disk-image path without `-device qemu-xhci` just logs "no xHCI" and continues to ATA).

- [ ] **Step 4: QEMU verify.** Build + boot with an xHCI:
```bash
make image64
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file=disk/image64.img,format=raw \
  -device qemu-xhci -display none -serial file:/tmp/x.log -no-reboot -d int -D /tmp/i.log &
sleep 12; grep -E "xHCI:|EXCEPTION|Triple" /tmp/x.log /tmp/i.log
```
Expected: `xHCI: N ports, M slots ...` line, NO exception/triple-fault.

- [ ] **Step 5: Commit.**
```bash
git add arch/x86_64/drivers/xhci_x86_64.* arch/x86_64/arch.mk kernel/Kernel.cpp
git commit -m "feat(usb,x86_64): xHCI PCI discovery + MMIO cap/op regs (QEMU-probed)"
```

### Task 4: xHCI controller init + command/event rings + port reset → device addressed (MD)

**Files:** Modify `arch/x86_64/drivers/xhci_x86_64.cpp`

> Implement the xHCI spec (rev 1.2) init sequence §4.2. Verification stays QEMU-smoke.

- [ ] **Step 1: Controller init sequence.** In order: wait `CNR`=0; `USBCMD.HCRST`=1, wait clear; set
  `CONFIG.MaxSlotsEn`; allocate the **DCBAA** (Device Context Base Address Array, `FrameAllocator` frame)
  → `DCBAAP`; allocate the **command ring** (frame, TRBs) → `CRCR` (with cycle bit); allocate the **event
  ring** + **ERST** (one segment) → `ERSTSZ/ERSTBA/ERDP` on interrupter 0; `USBCMD.RS`=1 (run).

- [ ] **Step 2: Port reset + enable.** Implement `enablePort(port)`: set `PORTSC.PR`, wait `PRC`; read
  speed from `PORTSC.PortSpeed`; issue **Enable Slot** command (command ring + wait its completion on the
  event ring) → slot id; allocate the device's **Input Context** + **Device Context**; set slot+EP0
  context (route, speed, max packet); issue **Address Device** command → device addressed. Return slot id.

- [ ] **Step 3: Control transfer on EP0.** Implement `submit()` for `USB_CONTROL`: build Setup/Data/Status
  TRBs on the slot's EP0 transfer ring, ring the doorbell, poll the event ring for the Transfer Event,
  fill `result`. (Bulk/interrupt EPs added in Phase 3/4 via `configureEndpoint`.)

- [ ] **Step 4: `usbHcRegister`** the xHCI as the controller, with an `UsbHcOps` whose `enablePort`/
  `submit`/`configureEndpoint`/`portCount` call the above.

- [ ] **Step 5: QEMU verify with a device.** Boot with `-device qemu-xhci -device usb-kbd`. Add a temporary
  bring-up call in `xhciInit` that enumerates port-by-port via `kernel::usbEnumeratePort` and logs
  `"USB dev VID:PID class=.."`. Expect the `usb-kbd`'s VID/PID + HID class logged, zero faults. Remove the
  temp enumeration loop before commit (the real enumeration driver lands in Phase 3/4).

- [ ] **Step 6: Commit.**
```bash
git add arch/x86_64/drivers/xhci_x86_64.cpp
git commit -m "feat(usb,x86_64): xHCI rings + port reset + Address Device + EP0 control (QEMU: enumerates usb-kbd)"
```

---

## Phase 2 — USB-HID kext → existing evdev → NanWM

### Task 5: HID report-descriptor parser + boot keyboard/mouse decoders (MI, host-tested)

**Files:**
- Create: `usb/UsbHid.h`, `usb/UsbHid.cpp`, `tests/test_usbhid.cpp`

- [ ] **Step 1: Failing test** — boot-protocol keyboard report → key down/up events; mouse report → rel
  move + buttons. The decoders take **sink callbacks** so the test captures events without the kernel.

```cpp
#include "doctest.h"
#include "UsbHid.h"
using namespace kernel;
TEST_CASE("boot keyboard report -> key events via sink") {
    struct Cap { int n; uint8_t code[8]; uint8_t down[8]; } cap{};
    auto sink=[](void* u,uint8_t keycode,uint8_t down){ auto c=(Cap*)u; c->code[c->n]=keycode; c->down[c->n]=down; c->n++; };
    HidKeyboard kb; kb.sink=sink; kb.user=&cap;
    // boot kbd report: [mods][reserved][6 keycodes]. Press 'A' (usage 0x04).
    uint8_t r1[8]={0,0, 0x04,0,0,0,0,0}; hidKeyboardReport(&kb, r1, 8);
    REQUIRE(cap.n==1); CHECK(cap.down[0]==1);
    uint8_t r2[8]={0,0, 0,0,0,0,0,0};    hidKeyboardReport(&kb, r2, 8);   // release
    REQUIRE(cap.n==2); CHECK(cap.down[1]==0); CHECK(cap.code[1]==cap.code[0]);
}
TEST_CASE("boot mouse report -> rel + buttons via sink") {
    struct Cap{int dx,dy,btn,syn;} cap{};
    HidMouse m; m.relSink=[](void*u,int dx,int dy){auto c=(Cap*)u;c->dx=dx;c->dy=dy;};
    m.btnSink=[](void*u,int b,int down){((Cap*)u)->btn=(down<<8)|b;};
    m.synSink=[](void*u){((Cap*)u)->syn++;}; m.user=&cap;
    uint8_t r[3]={0x01, 5, (uint8_t)-3}; hidMouseReport(&m, r, 3);   // button1 down, dx=5, dy=-3
    CHECK(cap.dx==5); CHECK(cap.dy==-3); CHECK(cap.syn==1);
}
```

- [ ] **Step 2: Run; expect FAIL** (`UsbHid.h` missing).

- [ ] **Step 3: Implement `usb/UsbHid.h` + `usb/UsbHid.cpp`.** Boot-protocol layouts are fixed (no report-
  descriptor parse needed for boot protocol): keyboard = 8-byte report (`[mods][rsvd][6 usages]`); mouse =
  3-byte report (`[buttons][dx][dy]`). Translate HID usage → the keycode space `KeyboardDevice` expects
  (provide a `hidUsageToKeycode[256]` table; map the common keys, the rest pass through). Diff successive
  keyboard reports to emit down/up edges. Emit mouse `dx/dy` as signed, then a SYN.

- [ ] **Step 4: Run; expect PASS.** Add to `TEST_MODULES`/`COV_PATTERNS` (already added `usb/UsbHid.cpp` in
  Task 2 Step 8 — verify it's there).

- [ ] **Step 5: Commit.**
```bash
git add usb/UsbHid.h usb/UsbHid.cpp tests/test_usbhid.cpp Makefile
git commit -m "feat(usb): HID boot keyboard/mouse decoders -> sink callbacks (host-tested)"
```

### Task 6: `usbhid` kext — bind HID interface, poll interrupt IN, feed evdev

**Files:**
- Create: `kext/usbhid/usbhid.cpp`
- Modify: `Makefile` (`X64` kext list + `_image64` install), `kernel/Kernel.cpp` (no change if kext
  auto-loads from `/nanos/kext`).

> Verification = QEMU (the kext loads from the mounted root, then USB kbd/mouse drive NanWM). Depends on
> Phase 3 (USB root mounted) for the kext to be loadable on a USB-only system — but on the QEMU
> disk-image path it loads from ATA, so HID can be verified independently with `-device usb-kbd`.

- [ ] **Step 1: Implement the kext entry** using the kext ABI (see `kext/e1000/e1000.cpp` +
  `kext/mouse/mouse_ps2.cpp` for the exact `knx_*` exports). On load: for each enumerated USB device with an
  interface class `USB_CLASS_HID`, `configureEndpoint` the interrupt IN endpoint, then in the kext's poll/IRQ
  hook, `submit` an interrupt-IN transfer for the report and:
  - keyboard (`bInterfaceProtocol==1`): feed `kernel::kbdFeed(keycode-or-scancode)` per the `HidKeyboard`
    sink → `/dev/input0` (the SAME sink PS/2 uses; NanWM unchanged).
  - mouse (`bInterfaceProtocol==2`): drive a `MouseDevice` via its `push(EV_REL/EV_KEY/EV_SYN,...)` from the
    `HidMouse` sinks; register it with `knx_add_input_dev` → `/dev/input<N>` (exactly like `mouse_ps2.cpp`).

- [ ] **Step 2: Build + install the kext.** Add `usbhid` to the x86_64 kext list and the `_image64` kext
  install loop (mirror how `mouse`/`e1000` kexts are built to `/nanos/kext/*.nkext`).

- [ ] **Step 3: QEMU verify (disk-image path, HID independent of USB-root).**
```bash
make image64
# inject a test authorized desktop autostart or use the console; boot with USB HID:
qemu-system-x86_64 -cpu qemu64 -m 512 -drive file=disk/image64.img,format=raw \
  -device qemu-xhci -device usb-mouse -device usb-kbd \
  -display none -serial file:/tmp/x.log -monitor unix:/tmp/qmon,server,nowait &
# start nwm via console sendkey, move the USB mouse via the QEMU monitor 'mouse_move',
# screendump and confirm the cursor/window responds; grep serial for kext load + zero faults.
```
Expected: `kext: usbhid.nkext ... loaded`, NanWM cursor moves with the USB mouse, no faults.

- [ ] **Step 4: Commit.**
```bash
git add kext/usbhid/usbhid.cpp Makefile
git commit -m "feat(usb): usbhid kext — USB kbd/mouse feed existing evdev (/dev/input*), NanWM unchanged"
```

---

## Phase 3 — USB Mass-Storage (read) + mount the USB root

### Task 7: BOT + SCSI read state machine (MI, host-tested)

**Files:**
- Create: `usb/UsbMsc.h`, `usb/UsbMsc.cpp`; extend `tests/usb_mock_hc.h` (script bulk IN/OUT); add cases to
  `tests/test_usbmsc.cpp`.

- [ ] **Step 1: Failing test** — `READ CAPACITY(10)` returns block count+size; `READ(10)` returns sector
  bytes. The mock HC answers bulk transfers from a scripted SCSI device (a small in-RAM "disk").

```cpp
#include "doctest.h"
#include "UsbMsc.h"
#include "usb_mock_hc.h"   // extended with a scripted SCSI target
using namespace kernel;
TEST_CASE("MSC READ CAPACITY + READ(10) over BOT") {
    mock::installScsiDisk(/*blocks=*/2048, /*blockSize=*/512, /*pattern=*/0xAB);
    UsbMsc msc; REQUIRE(usbMscInit(&msc, /*slot=*/1, /*epIn=*/0x81, /*epOut=*/0x02) == 0);
    uint32_t blocks=0, bsize=0; REQUIRE(usbMscReadCapacity(&msc,&blocks,&bsize)==0);
    CHECK(blocks==2048); CHECK(bsize==512);
    uint8_t buf[512]; REQUIRE(usbMscRead10(&msc, /*lba=*/0, /*count=*/1, buf)==0);
    CHECK(buf[0]==0xAB); CHECK(buf[511]==0xAB);
}
```

- [ ] **Step 2: Run; expect FAIL.**

- [ ] **Step 3: Implement `usb/UsbMsc.{h,cpp}`** — BOT: build a 31-byte **CBW** (Command Block Wrapper)
  with the SCSI CDB, bulk-OUT it; bulk-IN/OUT the data; bulk-IN the 13-byte **CSW** (Command Status
  Wrapper) and check status. SCSI CDBs: `READ CAPACITY(10)` (opcode 0x25, parse 8-byte reply →
  lastLBA+blockSize), `READ(10)` (opcode 0x28, big-endian LBA+count). All transfers go through
  `arch::usbHcOps()->submit` with `USB_BULK`.

- [ ] **Step 4: Run; expect PASS.** (Module already in `TEST_MODULES` from Task 2.)

- [ ] **Step 5: Commit.**
```bash
git add usb/UsbMsc.h usb/UsbMsc.cpp tests/test_usbmsc.cpp tests/usb_mock_hc.h
git commit -m "feat(usb): MSC Bulk-Only Transport + SCSI READ CAPACITY/READ(10) (host-tested)"
```

### Task 8: `UsbMscBlockDevice` (MI BlockDevice adapter) + register with DeviceManager

**Files:**
- Create: `drivers/UsbMscBlockDevice.h`, `drivers/UsbMscBlockDevice.cpp`; add a case to
  `tests/test_usbmsc.cpp`.

- [ ] **Step 1: Failing test** — `readSectors` over the mock SCSI disk returns the right bytes; `name()`/
  `sectorSize()` correct; `writeSectors` returns `-EROFS` (phase 1 read-only).

```cpp
TEST_CASE("UsbMscBlockDevice readSectors maps to READ(10)") {
    mock::installScsiDisk(2048, 512, 0xCD);
    UsbMsc msc; usbMscInit(&msc,1,0x81,0x02);
    UsbMscBlockDevice dev(&msc, "usb0");
    CHECK(dev.sectorSize()==512);
    uint8_t buf[512];
    CHECK(dev.readSectors(0,1,buf)==1);
    CHECK(buf[0]==0xCD);
    CHECK(dev.writeSectors(0,1,buf) < 0);   // read-only in phase 1
}
```

- [ ] **Step 2–4: Implement `UsbMscBlockDevice`** (subclass `BlockDevice`; `readSectors` → `usbMscRead10`
  per sector or batched; `writeSectors` → `-EROFS`; `sectorSize` from READ CAPACITY; `name` stored). Run;
  expect PASS.

- [ ] **Step 5: Commit.**
```bash
git add drivers/UsbMscBlockDevice.h drivers/UsbMscBlockDevice.cpp tests/test_usbmsc.cpp
git commit -m "feat(usb): UsbMscBlockDevice — BlockDevice over USB MSC (read-only, host-tested)"
```

### Task 9: Kernel storage bring-up — enumerate USB, register block device, mount USB root

**Files:** Modify `kernel/Kernel.cpp` (storage wiring), `arch/x86_64/drivers/xhci_x86_64.cpp` (bulk EP
support via `configureEndpoint`).

> Verification = QEMU booting with the root filesystem on a USB mass-storage device.

- [ ] **Step 1: Implement bulk endpoints in xHCI** `configureEndpoint` (bulk IN/OUT transfer rings +
  doorbells) so `UsbMsc` transfers work on real xHCI (the mock covered MI; this is the MD half).

- [ ] **Step 2: Kernel storage discovery.** In `Kernel::start`, after `xhciInit()`: for each USB port,
  `usbEnumeratePort`; if a device has a Mass-Storage interface, `usbMscInit` + `new UsbMscBlockDevice` +
  `DeviceManager::registerDevice`. Then **root discovery**: iterate registered block devices, probe each for
  a NanOS partition (the existing MBR/partition-probe used for ATA), mount the first match at
  `/disks/main`. Keep the ATA device + fallback so the QEMU `-drive` disk-image path is unchanged.

- [ ] **Step 3: Build the USB-root test image.** Reuse `disk/image64.img` as the USB drive backing
  file (it already has the full FS). Boot it as a USB mass-storage device instead of `-drive`:
```bash
make image64
qemu-system-x86_64 -cpu qemu64 -m 512 \
  -drive if=none,id=usbstick,file=disk/image64.img,format=raw \
  -device qemu-xhci -device usb-storage,drive=usbstick \
  -display none -serial file:/tmp/x.log -no-reboot -d int -D /tmp/i.log &
sleep 18; grep -E "Mounting ext|EXT-RW selftest|bash-5|EXCEPTION|Triple" /tmp/x.log /tmp/i.log
```
Expected: `/disks/main` mounts from the USB device, boot reaches the shell, zero faults. (GRUB itself
still boots the kernel from the `-drive`/firmware path in QEMU; the substance verified here is the
**kernel mounting root over USB-MSC**.)

- [ ] **Step 4: Commit.**
```bash
git add kernel/Kernel.cpp arch/x86_64/drivers/xhci_x86_64.cpp
git commit -m "feat(usb): mount the root filesystem from a USB mass-storage device (bulk EP + root discovery)"
```

---

## Phase 4 — Verification gate wiring

### Task 10: `smoke-usb` target + extend the x86_64 verification gate

**Files:** Modify `Makefile` (`smoke-usb`), `scripts/smoke-usb.sh` (new), wire into `verify64`.

- [ ] **Step 1: Write `scripts/smoke-usb.sh`** mirroring `scripts/smoke-x86_64.sh` but booting with
  `-device qemu-xhci -device usb-storage,drive=usbstick -device usb-kbd -device usb-mouse`; assert: USB
  root mounts, shell reached, `usbhid.nkext` loaded, console fork/exec OK, ZERO faults. Drive the USB mouse
  via the QEMU monitor and screendump NanWM to confirm USB input.

- [ ] **Step 2: Add `make smoke-usb`** (depends on `image64`) and add it to `verify64`:
  `verify64: test64 smoke-x86_64 smoke-usb`.

- [ ] **Step 3: Run it; expect PASS** (all assertions green).
```bash
make smoke-usb
```

- [ ] **Step 4: Commit.**
```bash
git add Makefile scripts/smoke-usb.sh
git commit -m "test(usb): smoke-usb gate — boot root-on-USB + USB HID in NanWM, wired into verify64"
```

---

## Phase 5 (follow-on) — SCSI WRITE(10): read-write USB root

### Task 11: Add `usbMscWrite10` + enable `UsbMscBlockDevice::writeSectors`

**Files:** Modify `usb/UsbMsc.{h,cpp}`, `drivers/UsbMscBlockDevice.cpp`, tests.

- [ ] **Step 1: Failing test** — `usbMscWrite10` then `usbMscRead10` round-trips bytes through the mock SCSI
  disk; `UsbMscBlockDevice::writeSectors` returns the count (no longer `-EROFS`).
- [ ] **Step 2–4:** Implement `WRITE(10)` (opcode 0x2A, bulk-OUT data); wire `writeSectors`. Run; expect PASS.
- [ ] **Step 5: QEMU verify** — the boot-time `extRwSelftest()` marker now persists across reboot on the
  USB root; `e2fsck -fn` of the image after a write session is clean.
- [ ] **Step 6: Commit.**
```bash
git add usb/UsbMsc.h usb/UsbMsc.cpp drivers/UsbMscBlockDevice.cpp tests/test_usbmsc.cpp
git commit -m "feat(usb): SCSI WRITE(10) — read-write USB root (e2fsck-clean, persists across reboot)"
```

---

## Notes for the implementer

- **xHCI is the hard part.** Implement strictly against the xHCI spec rev 1.2 §4.2 (init) and §4.9 (rings/
  TRBs). Build the MI core + mock-HC tests FIRST (Tasks 2,5,7,8) so the protocol logic is proven before you
  fight MMIO. The mock HC means enumeration/MSC/HID are fully validated without QEMU.
- **DMA:** every ring/context/data buffer must be a `FrameAllocator` frame (page-aligned, phys==virt). Never
  hand the controller a heap pointer that may straddle pages or move.
- **Poll model first:** `UsbTransfer.complete` is polled (xHCI `submit` runs to completion). An IRQ-driven
  event ring is a later optimization — not in this plan (YAGNI); the poll loop must yield
  (`Scheduler::ioWait()`) between event-ring checks so it doesn't starve the deferred-preemption scheduler.
- **check-arch:** `usb/` is MI — no `<arch/x86*>` headers, no port I/O, no inline asm there. Run
  `make check-arch` after MI edits.
- **Root-driver-in-kernel rule:** the xHCI+core+MSC path is compiled into the kernel (not a kext) precisely
  so a USB-only system can read its own root. Do NOT move it to a kext. (See spec §2.)
- **USB-as-root under pure UEFI** (ESP) is Stream D, out of scope; this plan's QEMU verification boots GRUB
  via the firmware/`-drive` path and proves the kernel-side USB-root mount.
