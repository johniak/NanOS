/*
 * Fb0Device.h — /dev/fb0 as a CharDevice over the kernel-mapped framebuffer.
 *
 * Thin glue: read/write copy framebuffer bytes, ioctl serves the fbdev screeninfo,
 * mmapInfo reports the LFB region so the kernel can map it into a process. The real
 * logic lives in Fbdev.cpp (host-tested); this is kernel-only (the LFB pointer is a
 * physical/identity address that has no host equivalent).
 */
#pragma once
#include "CharDevice.h"
#include "Fbdev.h"

namespace kernel {

class Fb0Device : public CharDevice {
	FbInfo m_fb;
	uint8_t* m_lfb;      // kernel virtual = identity-mapped physical framebuffer
	unsigned m_len;      // pitch * height
public:
	explicit Fb0Device(const FbInfo& fb);
	int read(unsigned off, void* buf, unsigned n);
	int write(unsigned off, const void* buf, unsigned n);
	int ioctl(unsigned cmd, void* arg);
	int mmapInfo(unsigned* physOut, unsigned* lenOut);
};

}  // namespace kernel
