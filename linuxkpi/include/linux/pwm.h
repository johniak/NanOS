/* linuxkpi/include/linux/pwm.h — PWM backlight. STUB (panel PWM backlight is a Dell follow-on; core
 * KMS does not need it). Inert accessors so intel_backlight compiles. */
#ifndef _LINUXKPI_LINUX_PWM_H
#define _LINUXKPI_LINUX_PWM_H
#include <linux/types.h>
struct device;
struct pwm_state { u64 period; u64 duty_cycle; unsigned polarity; bool enabled; };
struct pwm_device { struct pwm_state state; };
static inline bool pwm_enabled(struct pwm_device *p){ return p && p->state.enabled; }
static inline void pwm_get_state(const struct pwm_device *p, struct pwm_state *s){ if(p&&s)*s=p->state; }
static inline void pwm_init_state(const struct pwm_device *p, struct pwm_state *s){ if(p&&s)*s=p->state; }
static inline int pwm_apply_might_sleep(struct pwm_device *p, const struct pwm_state *s){ if(p&&s)p->state=*s; return 0; }
static inline struct pwm_device *pwm_get(struct device *dev, const char *con_id){ (void)dev;(void)con_id; return 0; }
static inline void pwm_put(struct pwm_device *p){ (void)p; }
static inline unsigned int pwm_get_relative_duty_cycle(const struct pwm_state *s, unsigned int scale)
{ return (s && s->period) ? (unsigned)((s->duty_cycle * scale) / s->period) : 0; }
static inline int pwm_set_relative_duty_cycle(struct pwm_state *s, unsigned int duty, unsigned int scale)
{ if(s&&scale) s->duty_cycle = (s->period * duty)/scale; return 0; }
#endif
