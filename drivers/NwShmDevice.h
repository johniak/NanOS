/*
 * NwShmDevice.h — /dev/nwshm: shared pixel buffers for the nanowm window pipeline.
 *
 * A GUI client allocates its window surface here (ioctl NWSHM_ALLOC → an opaque token),
 * mmap()s it, and hands the token to the compositor, which mmap()s the SAME physical pages
 * — window commits then carry coordinates only instead of pushing every pixel through the
 * request pipe. The backing store is a physically-contiguous frame-pool block (same pool as
 * GEM buffer objects), so CharDevice::mmapAt(token) can report one phys+len region.
 *
 * Trust model: any process that can open /dev/nwshm can map any live token (tokens are not
 * per-client). That matches /dev/fb0 (whole screen mappable, mode 0666) on this single-user
 * desktop; revisit both together when the desktop gets a real security boundary.
 *
 * Lifetime: buffers are freed by ioctl NWSHM_FREE (the compositor sends it when a window is
 * destroyed or its surface is replaced on resize). If both sides die without freeing, the
 * block leaks until reboot — the compositor's death ends the GUI session anyway.
 */
#pragma once
#include "CharDevice.h"
#include "Spinlock.h"

namespace kernel {

// ioctl commands ('NW' << 8 | n)
#define NWSHM_IOC_ALLOC  0x4E5701u   // arg = nwshm_ioc{bytes in; token out}
#define NWSHM_IOC_FREE   0x4E5702u   // arg = nwshm_ioc{token in}

struct nwshm_ioc {
	unsigned long long bytes;    // ALLOC in: requested size (page-rounded)
	unsigned long long token;    // ALLOC out / FREE in: opaque buffer handle
};

class NwShmDevice : public CharDevice {
	// Fixed slot table: 64 live buffers = 32 double-buffered windows, comfortably above
	// NW_MAX_WINDOWS. A slot's token is its phys base — opaque to userland, validated
	// against the table on every mmapAt/FREE so a stale/forged token cannot map freed pages.
	static const int MAX_BUFS = 64;
	struct Buf { unsigned long long phys, bytes; } m_buf[MAX_BUFS];
	Spinlock m_lock;   // SMP: nwm and clients ioctl concurrently
public:
	NwShmDevice();
	int read(unsigned off, void* buf, unsigned n);
	int write(unsigned off, const void* buf, unsigned n);
	int ioctl(unsigned cmd, void* arg);
	int mmapInfo(uint64_t* physOut, unsigned* lenOut);
	int mmapAt(uint64_t off, uint64_t* physOut, unsigned* lenOut);
};

}  // namespace kernel
