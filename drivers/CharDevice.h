/*
 * CharDevice.h — a minimal character device interface (machine-independent).
 *
 * VFS files are read/stat/readdir only; a character device additionally supports
 * write, ioctl, and reporting a physical region to mmap. SynthFs nodes of kind
 * SK_CHARDEV hold a CharDevice*; /dev/fb0 is the first one.
 */
#pragma once
#include <stdint.h>

namespace kernel {

struct WaitQueue;   // event wait list (kernel/WaitQueue.h); a streaming device may expose one

// poll(2) event/revent bits (Linux values), used by CharDevice::pollReady and the syscall
// layer. Defined here so device drivers can report readiness without pulling in Syscall.h.
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct CharDevice {
	virtual ~CharDevice() {}
	// Open/close accounting: the fd layer calls these as descriptors referencing this device are
	// created (open/dup/fork) and destroyed (close/exec-cloexec/exit). A streaming device that
	// pairs two endpoints (the pty) uses them to detect when one side has no open fds left, so the
	// other side's read can report EOF. Default no-op (most devices don't care).
	virtual void open() {}
	virtual void close() {}
	virtual int read(unsigned off, void* buf, unsigned n) = 0;
	virtual int write(unsigned off, const void* buf, unsigned n) = 0;
	virtual int ioctl(unsigned cmd, void* arg) = 0;
	// Report the device's physical region for mmap (phys base + byte length).
	// Returns 0 on success, <0 if the device is not mmappable. physOut is 64-bit so an
	// MMIO/framebuffer region above 4 GiB is reported without losing the high bits.
	virtual int mmapInfo(uint64_t* physOut, unsigned* lenOut) = 0;
	// Offset-aware mmap: map the object at device offset `off` (e.g. a GEM fake offset).
	// Default: only offset 0, backed by mmapInfo — existing devices (fb0, ...) keep working.
	virtual int mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut) {
		return off == 0 ? mmapInfo(physOut, lenOut) : -22;   // -EINVAL for nonzero offset
	}
	// poll() readiness: return the subset of `events` (POLLIN/POLLOUT bits) ready now.
	// Default = always ready (suits mmap devices like /dev/fb0); streaming devices (pty,
	// keyboard) override to report buffer state.
	virtual short pollReady(short events) { return events; }
	// The wait list a blocked reader/writer of this device parks on (woken when readiness
	// changes), or 0 if the device never blocks (always-ready mmap devices). Lets the syscall
	// dispatch sleep event-driven instead of re-polling the device every timer tick.
	virtual WaitQueue* waitQueue() { return 0; }
};

}  // namespace kernel
