/*
 * linuxkpi/include/linux/pm_qos.h — CPU latency QoS shim.
 *
 * i915 uses cpu_latency_qos_* to keep the CPU out of deep C-states while the GPU needs low
 * interrupt latency. NanOS has no PM-QoS framework and no cpuidle governor to honour it, so these
 * track the requested value but do not steer the hardware (CPU power is managed separately via
 * HWP/Speed-Shift). The request objects behave correctly so the driver's add/update/remove
 * bookkeeping is sound.
 */
#ifndef _LINUXKPI_LINUX_PM_QOS_H
#define _LINUXKPI_LINUX_PM_QOS_H

#include <linux/types.h>

#define PM_QOS_DEFAULT_VALUE      (-1)
#define PM_QOS_RESUME_LATENCY_NO_CONSTRAINT ((s32)(~0U >> 1))

struct pm_qos_request {
	s32  value;
	bool active;
};

static inline void cpu_latency_qos_add_request(struct pm_qos_request *req, s32 value)
{
	if (!req) return;
	req->value = value;
	req->active = true;
}

static inline void cpu_latency_qos_update_request(struct pm_qos_request *req, s32 new_value)
{
	if (req) req->value = new_value;
}

static inline void cpu_latency_qos_remove_request(struct pm_qos_request *req)
{
	if (!req) return;
	req->value = PM_QOS_DEFAULT_VALUE;
	req->active = false;
}

static inline int cpu_latency_qos_request_active(struct pm_qos_request *req)
{
	return req && req->active;
}

#endif /* _LINUXKPI_LINUX_PM_QOS_H */
