/*
 * Loopback.h — the loopback device "lo" (127.0.0.1/8). Its tx() re-injects the frame into the
 * RX backlog, so localhost traffic traverses the same Ethernet/IP demux as the wire — uniform
 * code paths, and a hardware-free way to exercise the whole stack in host tests.
 */
#pragma once

namespace kernel {

struct NetDevice;

// Allocate (static), configure and register the loopback device. Returns the lo NetDevice.
NetDevice* loopbackCreate();

}  // namespace kernel
