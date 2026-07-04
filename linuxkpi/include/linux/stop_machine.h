/* linuxkpi/include/linux/stop_machine.h — i915 reset uses stop_machine to quiesce CPUs. NanOS bring-up
 * is single-threaded/cooperative for reset, so the callback runs inline on the current CPU. */
#ifndef _LINUXKPI_LINUX_STOP_MACHINE_H
#define _LINUXKPI_LINUX_STOP_MACHINE_H
typedef int (*cpu_stop_fn_t)(void *arg);
static inline int stop_machine(cpu_stop_fn_t fn, void *data, const void *cpus)
{ (void)cpus; return fn ? fn(data) : 0; }
#endif
