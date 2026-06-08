/*
 * CharDevice.h — a minimal character device interface (machine-independent).
 *
 * VFS files are read/stat/readdir only; a character device additionally supports
 * write, ioctl, and reporting a physical region to mmap. SynthFs nodes of kind
 * SK_CHARDEV hold a CharDevice*; /dev/fb0 is the first one.
 */
#pragma once

namespace kernel {

struct CharDevice {
	virtual ~CharDevice() {}
	virtual int read(unsigned off, void* buf, unsigned n) = 0;
	virtual int write(unsigned off, const void* buf, unsigned n) = 0;
	virtual int ioctl(unsigned cmd, void* arg) = 0;
	// Report the device's physical region for mmap (phys base + byte length).
	// Returns 0 on success, <0 if the device is not mmappable.
	virtual int mmapInfo(unsigned* physOut, unsigned* lenOut) = 0;
};

}  // namespace kernel
