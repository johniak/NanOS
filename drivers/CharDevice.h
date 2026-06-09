/*
 * CharDevice.h — a minimal character device interface (machine-independent).
 *
 * VFS files are read/stat/readdir only; a character device additionally supports
 * write, ioctl, and reporting a physical region to mmap. SynthFs nodes of kind
 * SK_CHARDEV hold a CharDevice*; /dev/fb0 is the first one.
 */
#pragma once

namespace kernel {

// poll(2) event/revent bits (Linux values), used by CharDevice::pollReady and the syscall
// layer. Defined here so device drivers can report readiness without pulling in Syscall.h.
#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct CharDevice {
	virtual ~CharDevice() {}
	virtual int read(unsigned off, void* buf, unsigned n) = 0;
	virtual int write(unsigned off, const void* buf, unsigned n) = 0;
	virtual int ioctl(unsigned cmd, void* arg) = 0;
	// Report the device's physical region for mmap (phys base + byte length).
	// Returns 0 on success, <0 if the device is not mmappable.
	virtual int mmapInfo(unsigned* physOut, unsigned* lenOut) = 0;
	// poll() readiness: return the subset of `events` (POLLIN/POLLOUT bits) ready now.
	// Default = always ready (suits mmap devices like /dev/fb0); streaming devices (pty,
	// keyboard) override to report buffer state.
	virtual short pollReady(short events) { return events; }
};

}  // namespace kernel
