/*
 * VtTty.h — the /dev/ttyN character device over a kernel VtConsole (virtual terminal).
 *
 * One instance per node: /dev/tty1../dev/tty7 carry a fixed 1-based index; /dev/tty0 (index 0)
 * resolves to the ACTIVE VT at each call; /dev/tty (index -1) resolves to the caller's controlling
 * VT. read/write delegate to the VtConsole (write rasterizes iff that VT is live). ioctl implements
 * the Linux tty + VT control set (TIOCxxx, TCGETS/TCSETS, the VT_ and KD modes), so userland
 * (login, nwm) sees the real ABI.
 */
#pragma once
#include "CharDevice.h"

namespace kernel {

class VtConsole;

class VtTty : public CharDevice {
	int m_index;                 // 1..7 fixed; 0 = active VT; -1 = controlling VT (/dev/tty)
	int resolve() const;         // -> a concrete 1..7 VT index (or 0 if none)
public:
	explicit VtTty(int index) : m_index(index) {}
	int read(unsigned off, void* buf, unsigned n) override;
	int write(unsigned off, const void* buf, unsigned n) override;
	int ioctl(unsigned cmd, void* arg) override;
	int mmapInfo(uint64_t*, unsigned*) override { return -1; }
	short pollReady(short events) override;
	WaitQueue* waitQueue() override;
};

}  // namespace kernel
