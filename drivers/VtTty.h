/*
 * VtTty.h — the /dev/ttyN character device over a kernel VtConsole (virtual terminal).
 *
 * One instance per node: /dev/tty1../dev/tty7 carry a fixed 1-based index; /dev/tty0 (index 0)
 * resolves to the ACTIVE VT at each call. read/write delegate to the VtConsole (write rasterizes
 * iff that VT is live). ioctl implements the Linux tty + VT control set (TIOCxxx, TCGETS/TCSETS,
 * the VT_ and KD modes), so userland (login, nwm) sees the real ABI. TIOCSCTTY makes this VT the
 * caller's controlling terminal (Process::cttyDev).
 *
 * /dev/tty is a separate ControllingTty device that forwards every operation to the caller's
 * Process::cttyDev — the VtTty a getty adopted, OR the PtySlave an nterm/ssh shell adopted — so
 * /dev/tty resolves uniformly whether the controlling terminal is a VT or a pty.
 */
#pragma once
#include "CharDevice.h"

namespace kernel {

class VtConsole;

class VtTty : public CharDevice {
	int m_index;                 // 1..7 fixed; 0 = active VT
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

// /dev/tty — the caller's controlling terminal. Forwards every operation to Process::cttyDev (a
// VtTty or a PtySlave), set by TIOCSCTTY; returns -ENXIO when the caller has no controlling tty.
// open/close are NOT forwarded: opening /dev/tty must not bump the target's open accounting (a pty
// hangup is decided by the slave's own fds, not by /dev/tty references).
class ControllingTty : public CharDevice {
	CharDevice* target() const;   // the current process's cttyDev (never this), or 0
public:
	int read(unsigned off, void* buf, unsigned n) override;
	int write(unsigned off, const void* buf, unsigned n) override;
	int ioctl(unsigned cmd, void* arg) override;
	int mmapInfo(uint64_t* p, unsigned* l) override;
	short pollReady(short events) override;
	WaitQueue* waitQueue() override;
};

}  // namespace kernel
