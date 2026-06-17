#include "Fb0Device.h"

namespace kernel {

Fb0Device::Fb0Device(const FbInfo& fb)
	: m_fb(fb), m_lfb((uint8_t*) fb.phys), m_len(fb.pitch * fb.height) {}

int Fb0Device::read(unsigned off, void* buf, unsigned n) {
	return fbdevRead(m_lfb, m_len, off, buf, n);
}

int Fb0Device::write(unsigned off, const void* buf, unsigned n) {
	return fbdevWrite(m_lfb, m_len, off, buf, n);
}

int Fb0Device::ioctl(unsigned cmd, void* arg) {
	return fbdevIoctl(cmd, arg, m_fb);
}

int Fb0Device::mmapInfo(uint64_t* physOut, unsigned* lenOut) {
	*physOut = m_fb.phys;
	*lenOut = m_len;
	return 0;
}

}  // namespace kernel
