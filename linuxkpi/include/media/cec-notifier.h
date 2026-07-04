/* linuxkpi/include/media/cec-notifier.h — HDMI CEC notifier; NanOS has no CEC. Inert. */
#ifndef _LKPI_MEDIA_CEC_NOTIFIER_H
#define _LKPI_MEDIA_CEC_NOTIFIER_H
struct cec_notifier;
struct device;
struct edid;
/* CEC physical-address notify: no CEC adapter on NanOS, so these are inert. */
static inline void cec_notifier_set_phys_addr(struct cec_notifier *n, unsigned short pa){ (void)n;(void)pa; }
static inline void cec_notifier_set_phys_addr_from_edid(struct cec_notifier *n, const struct edid *edid){ (void)n;(void)edid; }
static inline void cec_notifier_phys_addr_invalidate(struct cec_notifier *n){ (void)n; }
#define CEC_PHYS_ADDR_INVALID 0xffff
#endif
