/*
 * Clock.h — machine-independent wall-clock accessor.
 *
 * The kernel seeds the boot epoch once from the platform RTC (arch::rtcEpoch) at startup; the
 * current wall-clock time is that epoch plus seconds-since-boot (the scheduler tick count). This
 * lives in the MI layer so filesystem code can timestamp inodes (mtime/ctime on write, dir ops,
 * utimes "now") without reaching the arch directly. Returns 0 until the boot epoch is set (e.g.
 * in host tests with no timer), which is harmless for on-disk timestamps.
 */
#ifndef KERNEL_CLOCK_H_
#define KERNEL_CLOCK_H_

namespace kernel {

void     setBootEpoch(unsigned epochSeconds);   // called once at boot with arch::rtcEpoch()
unsigned wallClockSeconds();                     // current wall-clock time in seconds since epoch

}  // namespace kernel

#endif /* KERNEL_CLOCK_H_ */
