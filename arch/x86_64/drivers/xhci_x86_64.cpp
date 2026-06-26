// arch/x86_64/drivers/xhci_x86_64.cpp — x86_64 xHCI driver (MD). See xhci_x86_64.h.
//
// Implements <arch/usbhc.h> against the xHCI spec rev 1.2: PCI discovery + MMIO (§5.3), the
// controller init sequence (§4.2), command/event rings + ERST (§4.9), port reset, slot/endpoint
// contexts + Enable Slot / Address Device (§4.3, §4.6), and EP0 control transfers (§4.11). Poll
// model: submit() runs a transfer to completion by polling the event ring (no IRQ yet; YAGNI).
// Ring-0 MD code, so it does its own volatile MMIO + DMA (FrameAllocator frames; phys == virt).
#include "xhci_x86_64.h"
#include <arch/usbhc.h>
#include <arch/mmu.h>
#include "Pci.h"
#include "Console.h"
#include "FrameAllocator.h"
#include "Spinlock.h"
#include <stdint.h>
#include <string.h>

namespace arch {

using kernel::Console;

namespace {

using kernel::Pci;
using kernel::PciDevice;
using kernel::g_frames;

// Serializes ALL controller access (TRB rings + the single shared event ring + doorbells). Without
// it, an MSC bulk read (xhciSubmit, on any CPU) and the USB-HID poll thread (xhciIntPoll, every
// tick) consume the SAME event ring concurrently under SMP: one steals the other's completion and
// both race the dequeue pointer, so a file read returns corrupted bytes. Single-CPU hid this (no
// true concurrency), so it only bit on real multi-core hardware booting root-on-USB. A plain ticket
// Spinlock is zero-init safe as a file-scope global (no global ctors run on NanOS); SpinGuard keeps
// IRQs enabled across the (potentially long) event poll, like the block cache's device-I/O lock.
kernel::Spinlock g_xhciLock;
using kernel::SpinGuard;

// ---- register offsets ----
// Capability registers (bytes from g_mmio).
enum { CAP_HCSPARAMS1 = 0x04, CAP_HCSPARAMS2 = 0x08, CAP_HCCPARAMS1 = 0x10,
       CAP_DBOFF = 0x14, CAP_RTSOFF = 0x18 };
// Operational registers (bytes from g_op).
enum { OP_USBCMD = 0x00, OP_USBSTS = 0x04, OP_PAGESIZE = 0x08, OP_CRCR = 0x18,
       OP_DCBAAP = 0x30, OP_CONFIG = 0x38, OP_PORTSC = 0x400 };
enum { USBCMD_RS = 0x1, USBCMD_HCRST = 0x2 };
enum { USBSTS_HCH = 0x1, USBSTS_CNR = 0x800 };
// PORTSC bits.
enum { PORTSC_CCS = 0x1, PORTSC_PED = 0x2, PORTSC_PR = 0x10, PORTSC_PP = 0x200, PORTSC_PRC = 0x200000 };
const uint32_t PORTSC_RW1C = 0xFE0000;   // CSC..CEC change bits (write-1-to-clear); preserve on RMW.
// Runtime interrupter-0 registers (bytes from g_rt).
enum { RT_IR0 = 0x20, IR_ERSTSZ = 0x08, IR_ERSTBA = 0x10, IR_ERDP = 0x18 };
// TRB types.
enum { TRB_NORMAL = 1, TRB_LINK = 6, TRB_ENABLE_SLOT = 9, TRB_ADDRESS_DEVICE = 11,
       TRB_CONFIGURE_EP = 12, TRB_EVAL_CONTEXT = 13,
       TRB_SETUP = 2, TRB_DATA = 3, TRB_STATUS = 4,
       TRB_TRANSFER_EVENT = 32, TRB_CMD_COMPLETION = 33, TRB_PORT_STATUS = 34 };

struct Trb { uint64_t param; uint32_t status; uint32_t control; } __attribute__((packed));

const int RING_TRBS = 64;   // per ring (1 KiB); last entry is a Link TRB back to start.
const int EVT_TRBS  = 64;   // event ring segment size.
const int MAX_SLOTS = 16;   // tracked device slots.

struct EpRing { Trb* ring; int enq; int cycle;
                uint64_t armed; bool ready; bool err; int residual; };   // intPoll: outstanding TRB + completion
struct SlotState { uint8_t* devCtx; uint8_t* inputCtx; EpRing ep[32]; };

volatile uint8_t* g_mmio = 0;
volatile uint8_t* g_op   = 0;
volatile uint8_t* g_rt   = 0;
volatile uint32_t* g_db  = 0;     // doorbell array
uint32_t g_mmioPhys = 0;
int g_maxPorts = 0, g_maxSlots = 0;
int g_ctxSize  = 32;              // 32 or 64 (HCCPARAMS1.CSZ)

uint64_t* g_dcbaa = 0;
EpRing    g_cmd{};                // command ring
Trb*      g_evt = 0;              // event ring segment
int       g_evtDeq = 0, g_evtCycle = 1;
SlotState g_slots[MAX_SLOTS]{};

inline uint32_t cap32(int off) { return *(volatile uint32_t*) (g_mmio + off); }
inline uint32_t op32(int off)  { return *(volatile uint32_t*) (g_op + off); }
inline void     wop32(int off, uint32_t v) { *(volatile uint32_t*) (g_op + off) = v; }
inline void     wop64(int off, uint64_t v) { *(volatile uint64_t*) (g_op + off) = v; }
inline uint32_t rt32(int off)  { return *(volatile uint32_t*) (g_rt + off); }
inline void     wrt32(int off, uint32_t v) { *(volatile uint32_t*) (g_rt + off) = v; }
inline void     wrt64(int off, uint64_t v) { *(volatile uint64_t*) (g_rt + off) = v; }
inline uint32_t portsc(int port)        { return op32(OP_PORTSC + (port-1)*0x10); }
inline void     wportsc(int port, uint32_t v) {
    // Preserve PED + the RW1C change bits when setting other fields (writing them 1 disables/clears).
    uint32_t cur = portsc(port) & ~(PORTSC_RW1C | PORTSC_PED);
    *(volatile uint32_t*) (g_op + OP_PORTSC + (port-1)*0x10) = cur | v;
}

inline uint64_t phys(void* p) { return (uint64_t)(uintptr_t) p; }
uint8_t* allocFrame() { uint64_t f = g_frames.alloc(); if (f) memset((void*)(uintptr_t)f, 0, kernel::FRAME_SIZE); return (uint8_t*)(uintptr_t)f; }

void ioWaitSpin() { for (volatile int i = 0; i < 1000; i++) {} }

// Push one TRB onto a ring; returns the physical address of the slot it was written to.
// Handles the trailing Link TRB (wrap + toggle cycle).
uint64_t ringPush(EpRing& r, uint64_t param, uint32_t status, uint32_t control) {
    Trb* t = &r.ring[r.enq];
    uint64_t slotPhys = phys(t);
    t->param = param; t->status = status;
    t->control = control | (uint32_t)(r.cycle & 1);
    r.enq++;
    if (r.enq == RING_TRBS - 1) {
        Trb* lnk = &r.ring[r.enq];
        lnk->param = phys(r.ring); lnk->status = 0;
        lnk->control = (TRB_LINK << 10) | (1 << 1) /*Toggle Cycle*/ | (uint32_t)(r.cycle & 1);
        r.enq = 0; r.cycle ^= 1;
    }
    return slotPhys;
}

// Poll the event ring for the next event; returns false on timeout.
bool eventPoll(Trb* out) {
    for (long guard = 0; guard < 20000000L; guard++) {
        Trb* e = &g_evt[g_evtDeq];
        if ((int)(e->control & 1) == g_evtCycle) {
            *out = *e;
            g_evtDeq++;
            if (g_evtDeq == EVT_TRBS) { g_evtDeq = 0; g_evtCycle ^= 1; }
            uint64_t deqPhys = phys(&g_evt[g_evtDeq]);
            wrt64(RT_IR0 + IR_ERDP, deqPhys | (1ull << 3) /*EHB clear*/);
            return true;
        }
        ioWaitSpin();
    }
    return false;
}

// Non-blocking single event check: returns true + the event if one is ready, false otherwise.
bool eventPollNB(Trb* out) {
    Trb* e = &g_evt[g_evtDeq];
    if ((int)(e->control & 1) != g_evtCycle) return false;
    *out = *e;
    g_evtDeq++;
    if (g_evtDeq == EVT_TRBS) { g_evtDeq = 0; g_evtCycle ^= 1; }
    wrt64(RT_IR0 + IR_ERDP, phys(&g_evt[g_evtDeq]) | (1ull << 3));
    return true;
}

// Execute a command-ring command; returns completion code (1 = success) and slot id via *slot.
int cmdExec(uint64_t param, uint32_t control, int* slot) {
    ringPush(g_cmd, param, 0, control);
    g_db[0] = 0;   // command-ring doorbell (target 0)
    Trb ev;
    for (int guard = 0; guard < 16; guard++) {
        if (!eventPoll(&ev)) return -1;
        if (((ev.control >> 10) & 0x3F) == TRB_CMD_COMPLETION) {
            if (slot) *slot = (ev.control >> 24) & 0xFF;
            return (ev.status >> 24) & 0xFF;
        }
    }
    return -1;
}

uint32_t* ctxAt(uint8_t* base, int index) { return (uint32_t*) (base + index * g_ctxSize); }

// Map a USB endpoint address (0 = default control) to its Device Context Index.
int dciOf(int endpoint) {
    if (endpoint == 0) return 1;                 // EP0 control
    int num = endpoint & 0x0F;
    int in  = (endpoint & 0x80) ? 1 : 0;
    return num * 2 + in;
}

// xHCI EP Type field (EP Context dword1 bits 5:3).
int epTypeOf(UsbXfer type, UsbDir dir) {
    if (type == USB_BULK) return dir == USB_IN ? 6 : 2;
    if (type == USB_INT)  return dir == USB_IN ? 7 : 3;
    return 4;   // control
}

// xHCI Extended Capability: USB Legacy Support (id 1). USBLEGSUP bit16=BIOS owned, bit24=OS owned;
// USBLEGCTLSTS (+4) holds SMI enables (low) + RW1C SMI status (high). Claim OS ownership, wait for
// BIOS to release (bounded), then silence the SMIs. No Legacy cap (e.g. QEMU) => no-op.
void biosHandoff() {
    uint32_t hcc1 = cap32(CAP_HCCPARAMS1);
    uint32_t xecp = (hcc1 >> 16) & 0xFFFF;            // xECP: dword offset from MMIO base
    if (!xecp) return;
    volatile uint32_t* cap = (volatile uint32_t*) (g_mmio + (uintptr_t)xecp * 4);
    for (int guard = 0; guard < 256; guard++) {
        uint32_t c = cap[0];
        uint8_t id = (uint8_t) (c & 0xFF);
        if (id == 1) {                                 // USB Legacy Support
            cap[0] = c | (1u << 24);                   // HC OS Owned Semaphore
            for (int i = 0; i < 1000000 && (cap[0] & (1u << 16)); i++) ioWaitSpin();
            uint32_t enableMask = (1u<<0)|(1u<<4)|(1u<<13)|(1u<<14)|(1u<<15);   // SMI enables
            uint32_t rw1cMask   = (1u<<20)|(1u<<29)|(1u<<30)|(1u<<31);          // SMI status (write 1 to clear)
            cap[1] = (cap[1] & ~enableMask) | rw1cMask;
            return;
        }
        uint8_t next = (uint8_t) ((c >> 8) & 0xFF);    // next: dword stride (0 = end)
        if (!next) return;
        cap = (volatile uint32_t*) ((volatile uint8_t*) cap + (uintptr_t)next * 4);
    }
}

// Intel chipsets (vendor 0x8086) share USB2 ports between EHCI and xHCI; route them to xHCI. Harmless
// on EHCI-less PCHs (Skylake+, incl. the Comet Lake target). Guarded so it never touches non-Intel HW.
void intelPortRoute(const PciDevice& d) {
    if (d.vendor != 0x8086) return;
    Pci::write32(d.bus, d.dev, d.func, 0xD8, 0xFFFFFFFFu);   // XUSB2PR: USB2 ports -> xHCI
    Pci::write32(d.bus, d.dev, d.func, 0xD0, 0xFFFFFFFFu);   // USB3_PSSEN: enable SuperSpeed
}

// ---- controller bring-up ----
void controllerInit() {
    while (op32(OP_USBSTS) & USBSTS_CNR) ioWaitSpin();        // wait Controller Not Ready clear
    wop32(OP_USBCMD, op32(OP_USBCMD) & ~USBCMD_RS);            // stop
    while (!(op32(OP_USBSTS) & USBSTS_HCH)) ioWaitSpin();      // wait halted
    wop32(OP_USBCMD, USBCMD_HCRST);                            // reset
    while (op32(OP_USBCMD) & USBCMD_HCRST) ioWaitSpin();
    while (op32(OP_USBSTS) & USBSTS_CNR) ioWaitSpin();

    wop32(OP_CONFIG, g_maxSlots);                             // MaxSlotsEn

    // DCBAA (Device Context Base Address Array).
    g_dcbaa = (uint64_t*) allocFrame();
    // Scratchpad buffers, if the controller demands them.
    uint32_t hcs2 = cap32(CAP_HCSPARAMS2);
    int maxScratch = (int)((((hcs2 >> 21) & 0x1F) << 5) | ((hcs2 >> 27) & 0x1F));
    if (maxScratch > 0) {
        uint64_t* arr = (uint64_t*) allocFrame();
        for (int i = 0; i < maxScratch; i++) arr[i] = phys(allocFrame());
        g_dcbaa[0] = phys(arr);
    }
    wop64(OP_DCBAAP, phys(g_dcbaa));

    // Command ring.
    g_cmd.ring = (Trb*) allocFrame(); g_cmd.enq = 0; g_cmd.cycle = 1;
    wop64(OP_CRCR, phys(g_cmd.ring) | 1 /*RCS*/);

    // Event ring + ERST (one segment).
    g_evt = (Trb*) allocFrame(); g_evtDeq = 0; g_evtCycle = 1;
    uint64_t* erst = (uint64_t*) allocFrame();
    erst[0] = phys(g_evt);                  // segment base
    erst[1] = (uint64_t) EVT_TRBS;          // segment size (TRBs), high dword 0
    wrt32(RT_IR0 + IR_ERSTSZ, 1);
    wrt64(RT_IR0 + IR_ERDP, phys(g_evt));
    wrt64(RT_IR0 + IR_ERSTBA, phys(erst));

    wop32(OP_USBCMD, op32(OP_USBCMD) | USBCMD_RS);            // run
    while (op32(OP_USBSTS) & USBSTS_HCH) ioWaitSpin();
}

// Build the EP0 context (DCI 1) for a slot with a given max packet size.
void writeEp0Context(uint32_t* ep0, uint64_t ringPhys, int maxPacket) {
    ep0[0] = 0;
    ep0[1] = (4 << 3) /*EP Type = Control*/ | (3 << 1) /*CErr*/ | ((uint32_t)maxPacket << 16);
    ep0[2] = (uint32_t)(ringPhys & ~0xFull) | 1 /*DCS*/;
    ep0[3] = (uint32_t)(ringPhys >> 32);
    ep0[4] = 8;   // Average TRB Length
}

int initialMaxPacket(int speed) {
    switch (speed) { case 4: return 512; case 3: return 64; case 2: return 8; default: return 8; }
}

// One internal control GET_DESCRIPTOR(DEVICE) of `len` bytes during enumeration setup.
int controlIn(int slotId, uint8_t descType, void* buf, int len);   // fwd (defined via submit path)

// Reset+enable `port`, allocate a slot, address the device, fix EP0 max packet. Returns slot id.
int xhciEnablePort(UsbHc*, int port, int* speedOut) {
    uint32_t ps = portsc(port);
    if (!(ps & PORTSC_CCS)) return -1;                   // nothing connected
    if (!(ps & PORTSC_PED)) {                            // USB2: drive a reset; USB3 auto-enables
        wportsc(port, PORTSC_PR);
        for (int i = 0; i < 100000 && !(portsc(port) & PORTSC_PRC); i++) ioWaitSpin();
        wportsc(port, PORTSC_PRC);                       // clear reset-change
    }
    ps = portsc(port);
    int speed = (ps >> 10) & 0xF;
    if (speedOut) *speedOut = speed;

    int slot = 0;
    if (cmdExec(0, (TRB_ENABLE_SLOT << 10), &slot) != 1 || slot <= 0 || slot >= MAX_SLOTS) return -1;
    SlotState& s = g_slots[slot];

    s.devCtx = allocFrame();
    g_dcbaa[slot] = phys(s.devCtx);
    EpRing& ep0 = s.ep[1];
    ep0.ring = (Trb*) allocFrame(); ep0.enq = 0; ep0.cycle = 1;

    int mp = initialMaxPacket(speed);
    s.inputCtx = allocFrame();
    ctxAt(s.inputCtx, 0)[1] = (1 << 0) | (1 << 1);       // Input Control: add Slot + EP0
    uint32_t* slotCtx = ctxAt(s.inputCtx, 1);
    slotCtx[0] = (1u << 27) /*context entries = 1*/ | ((uint32_t)speed << 20);
    slotCtx[1] = ((uint32_t)port << 16) /*root hub port*/;
    writeEp0Context(ctxAt(s.inputCtx, 2), phys(ep0.ring), mp);

    if (cmdExec(phys(s.inputCtx), (TRB_ADDRESS_DEVICE << 10) | ((uint32_t)slot << 24), 0) != 1) return -1;

    // Read the first 8 bytes of the device descriptor to learn the real EP0 max packet size;
    // re-evaluate the EP0 context if it differs (mandatory for full-speed: 8/16/32/64).
    uint8_t dd8[8] = {0};
    if (controlIn(slot, /*USB_DT_DEVICE*/1, dd8, 8) >= 8) {
        int realMp = (speed == 4) ? 512 : dd8[7];
        if (realMp > 0 && realMp != mp) {
            memset(s.inputCtx, 0, kernel::FRAME_SIZE);
            ctxAt(s.inputCtx, 0)[1] = (1 << 1);          // add EP0 only
            writeEp0Context(ctxAt(s.inputCtx, 2), phys(ep0.ring), realMp);
            cmdExec(phys(s.inputCtx), (TRB_EVAL_CONTEXT << 10) | ((uint32_t)slot << 24), 0);
        }
    }
    return slot;
}

int xhciSubmit(UsbHc*, UsbTransfer* t) {
    SpinGuard _xg(g_xhciLock);   // serialize TRB submit + event-ring poll vs the HID poll thread
    if (t->slot <= 0 || t->slot >= MAX_SLOTS) { t->result = -1; t->complete = 1; return -1; }
    SlotState& s = g_slots[t->slot];
    int dci = dciOf(t->endpoint);
    EpRing& r = s.ep[dci];
    if (!r.ring) { t->result = -1; t->complete = 1; return -1; }

    if (t->type == USB_CONTROL) {
        uint64_t setupData = 0; memcpy(&setupData, &t->setup, 8);
        uint32_t trt = (t->len == 0) ? 0 : (t->dir == USB_IN ? 3 : 2);
        ringPush(r, setupData, 8, (TRB_SETUP << 10) | (1 << 6) /*IDT*/ | (trt << 16));
        uint64_t dataPhys = 0;
        if (t->len) dataPhys = ringPush(r, phys(t->data), t->len,
                                        (TRB_DATA << 10) | (1 << 2) /*ISP*/ | ((t->dir == USB_IN ? 1u : 0u) << 16));
        uint32_t sdir = (t->len && t->dir == USB_IN) ? 0 : 1;
        uint64_t statusPhys = ringPush(r, 0, 0, (TRB_STATUS << 10) | (sdir << 16) | (1 << 5) /*IOC*/);
        g_db[t->slot] = (uint32_t)dci;

        int dataLen = (int)t->len;
        Trb ev;
        for (int guard = 0; guard < 16; guard++) {
            if (!eventPoll(&ev)) { t->result = -1; t->complete = 1; return -1; }
            if (((ev.control >> 10) & 0x3F) != TRB_TRANSFER_EVENT) continue;
            int cc = (ev.status >> 24) & 0xFF;
            if (ev.param == dataPhys) dataLen = (int)t->len - (int)(ev.status & 0xFFFFFF);
            if (ev.param == statusPhys) {
                t->result = (cc == 1) ? dataLen : -1;
                t->complete = 1;
                return t->result < 0 ? -1 : 0;
            }
            if (cc != 1 && cc != 13 /*short packet*/) { t->result = -1; t->complete = 1; return -1; }
        }
        t->result = -1; t->complete = 1; return -1;
    }
    // Bulk / interrupt: a single Normal TRB carrying the data buffer.
    uint64_t trbPhys = ringPush(r, phys(t->data), t->len,
                                (TRB_NORMAL << 10) | (1 << 2) /*ISP*/ | (1 << 5) /*IOC*/);
    g_db[t->slot] = (uint32_t)dci;
    Trb ev;
    for (int guard = 0; guard < 16; guard++) {
        if (!eventPoll(&ev)) { t->result = -1; t->complete = 1; return -1; }
        if (((ev.control >> 10) & 0x3F) != TRB_TRANSFER_EVENT) continue;
        int cc = (ev.status >> 24) & 0xFF;
        if (ev.param == trbPhys) {
            t->result = (cc == 1 || cc == 13) ? (int)(t->len - (ev.status & 0xFFFFFF)) : -1;
            t->complete = 1;
            return t->result < 0 ? -1 : 0;
        }
        if (cc != 1 && cc != 13) { t->result = -1; t->complete = 1; return -1; }
    }
    t->result = -1; t->complete = 1; return -1;
}

// Configure a bulk/interrupt endpoint: allocate its transfer ring, set its EP context, and issue
// a Configure Endpoint command (bumping the slot's Context Entries to cover the new DCI).
// Drain all currently-pending transfer events into the per-EP completion records (so events for
// any armed interrupt endpoint are captured regardless of which EP the caller is polling).
void drainTransferEvents() {
    Trb ev;
    while (eventPollNB(&ev)) {
        if (((ev.control >> 10) & 0x3F) != TRB_TRANSFER_EVENT) continue;
        for (int s = 0; s < MAX_SLOTS; s++)
            for (int d = 0; d < 32; d++) {
                EpRing& r = g_slots[s].ep[d];
                if (r.armed && r.armed == ev.param) {
                    int cc = (ev.status >> 24) & 0xFF;
                    r.err = (cc != 1 && cc != 13);            // 1=success, 13=short packet
                    r.residual = (int)(ev.status & 0xFFFFFF);
                    r.ready = true; r.armed = 0;
                }
            }
    }
}

// Non-blocking interrupt-IN poll: arm one Normal TRB if none outstanding; return bytes once a
// report has arrived (re-arming next call), 0 if not ready, <0 on error.
int xhciIntPoll(UsbHc*, UsbTransfer* t) {
    SpinGuard _xg(g_xhciLock);   // drains the shared event ring — must not race MSC xhciSubmit
    if (t->slot <= 0 || t->slot >= MAX_SLOTS) return -1;
    SlotState& s = g_slots[t->slot];
    int dci = dciOf(t->endpoint);
    EpRing& r = s.ep[dci];
    if (!r.ring) return -1;
    drainTransferEvents();
    if (r.ready) {
        r.ready = false;
        if (r.err) { t->result = -1; t->complete = 1; return -1; }
        int n = (int)t->len - r.residual;
        t->result = n; t->complete = 1;
        return n;
    }
    if (!r.armed) {
        r.armed = ringPush(r, phys(t->data), t->len, (TRB_NORMAL << 10) | (1 << 2) | (1 << 5));
        g_db[t->slot] = (uint32_t)dci;
    }
    return 0;
}

int xhciConfigureEndpoint(UsbHc*, int slot, int endpoint, UsbXfer type, UsbDir dir, int maxPacket) {
    SpinGuard _xg(g_xhciLock);   // issues a Configure-EP command + drains events: serialize the ring
    if (slot <= 0 || slot >= MAX_SLOTS) return -1;
    SlotState& s = g_slots[slot];
    int dci = dciOf(endpoint);
    if (dci < 2 || dci >= 32) return -1;
    if (!s.ep[dci].ring) { s.ep[dci].ring = (Trb*) allocFrame(); s.ep[dci].enq = 0; s.ep[dci].cycle = 1; }

    memset(s.inputCtx, 0, kernel::FRAME_SIZE);
    ctxAt(s.inputCtx, 0)[1] = (1u << 0) | (1u << dci);          // add Slot + the new EP
    uint32_t* slotIn  = ctxAt(s.inputCtx, 1);
    uint32_t* slotDev = ctxAt(s.devCtx, 0);
    slotIn[0] = (slotDev[0] & ~(0x1Fu << 27)) | ((uint32_t)dci << 27);   // Context Entries = dci
    slotIn[1] = slotDev[1];
    uint32_t* ep = ctxAt(s.inputCtx, dci + 1);
    // Interval (EP Context dword0 bits 23:16): period = 2^Interval * 125us microframes. Interrupt
    // endpoints MUST have a valid interval or the controller never schedules periodic transfers;
    // ~8ms (Interval=6) suits a boot keyboard/mouse. Bulk ignores this field.
    ep[0] = (type == USB_INT) ? (6u << 16) : 0;
    ep[1] = ((uint32_t)epTypeOf(type, dir) << 3) | (3 << 1) /*CErr*/ | ((uint32_t)maxPacket << 16);
    ep[2] = (uint32_t)(phys(s.ep[dci].ring) & ~0xFull) | 1 /*DCS*/;
    ep[3] = (uint32_t)(phys(s.ep[dci].ring) >> 32);
    ep[4] = (uint32_t)maxPacket;   // Average TRB Length
    return cmdExec(phys(s.inputCtx), (TRB_CONFIGURE_EP << 10) | ((uint32_t)slot << 24), 0) == 1 ? 0 : -1;
}
int xhciPortCount(UsbHc*) { return g_maxPorts; }

const UsbHcOps OPS = { xhciEnablePort, xhciConfigureEndpoint, xhciSubmit, xhciPortCount, xhciIntPoll };

// Defined here (after xhciSubmit) so enablePort's EP0 fix-up can issue a control IN.
int controlIn(int slotId, uint8_t descType, void* buf, int len) {
    UsbTransfer t{}; t.slot = slotId; t.endpoint = 0; t.type = USB_CONTROL; t.dir = USB_IN;
    t.setup = { 0x80, 6 /*GET_DESCRIPTOR*/, (uint16_t)(descType << 8), 0, (uint16_t)len };
    t.data = buf; t.len = (uint32_t)len;
    xhciSubmit(0, &t);
    return t.result;
}

// Locate the first xHCI controller: PCI class 0x0C / subclass 0x03 / prog-IF 0x30.
bool findXhci(PciDevice& out) {
    PciDevice devs[32];
    int n = Pci::enumerate(devs, 32);
    for (int i = 0; i < n; i++)
        if (devs[i].classCode == 0x0C && devs[i].subclass == 0x03 && devs[i].progIf == 0x30) {
            out = devs[i];
            return true;
        }
    return false;
}

}  // namespace

void xhciInit() {
    PciDevice d;
    if (!findXhci(d)) {
        Console::writeLine("xHCI: none (continuing on ATA)");
        return;
    }
    g_mmioPhys = d.bar[0].addr;
    uint32_t bytes = d.bar[0].size ? d.bar[0].size : 0x1000;
    Pci::enableMemSpace(d);
    Pci::enableBusMaster(d);
    mmuMapKernelMmio(g_mmioPhys, bytes);
    g_mmio = (volatile uint8_t*) (uintptr_t) g_mmioPhys;

    // QEMU's xHCI MMIO services only 32-bit accesses to the capability block, so read the whole
    // CAPLENGTH/HCIVERSION dword and slice it (a sub-dword read returns 0).
    uint32_t cap0      = cap32(0x00);
    uint8_t  capLength = (uint8_t)  (cap0 & 0xFF);
    uint16_t hciVer    = (uint16_t) ((cap0 >> 16) & 0xFFFF);
    uint32_t hcs1      = cap32(CAP_HCSPARAMS1);
    uint32_t hcc1      = cap32(CAP_HCCPARAMS1);
    g_op       = g_mmio + capLength;
    g_rt       = g_mmio + (cap32(CAP_RTSOFF) & ~0x1Fu);
    g_db       = (volatile uint32_t*) (g_mmio + (cap32(CAP_DBOFF) & ~0x3u));
    g_maxSlots = (int) (hcs1 & 0xFF); if (g_maxSlots >= MAX_SLOTS) g_maxSlots = MAX_SLOTS - 1;
    g_maxPorts = (int) ((hcs1 >> 24) & 0xFF);
    g_ctxSize  = (hcc1 & 0x4) ? 64 : 32;

    Console::write("xHCI: ");
    Console::write(g_maxPorts);   Console::write(" ports, ");
    Console::write(g_maxSlots);   Console::write(" slots, HCIVERSION=");
    Console::writeHex((uint64_t) hciVer);
    Console::write(" @ BAR0=");
    Console::writeHex((uint64_t) g_mmioPhys);
    Console::writeLine("");

    biosHandoff();          // take the controller from BIOS/SMM (no-op on QEMU)
    intelPortRoute(d);      // route USB2 ports to xHCI on Intel (no-op elsewhere)
    controllerInit();

    // Real xHCI hardware powers root-hub ports OFF until software sets Port Power (PP), and a
    // freshly connected device needs a debounce interval before it asserts Connect Status (CCS).
    // QEMU powers ports and sets CCS instantly, so this was never needed there — on the Dell it is
    // why enumeration found 0 devices (the very stick we booted from). Power every port, then poll
    // for a connection to settle before usbEnumerateAll() runs.
    for (int p = 1; p <= g_maxPorts; p++)
        wportsc(p, PORTSC_PP);
    for (int tries = 0; tries < 300; tries++) {
        bool any = false;
        for (int p = 1; p <= g_maxPorts; p++)
            if (portsc(p) & PORTSC_CCS) { any = true; break; }
        if (any) break;
        for (int k = 0; k < 20000; k++) ioWaitSpin();
    }

    usbHcRegister((UsbHc*) g_mmio, &OPS);
}

void usbHostInit() { xhciInit(); }

}  // namespace arch
