/*
 * linuxkpi/include/linux/pinctrl/consumer.h — pinctrl consumer API.
 *
 * devm_pinctrl_get_select selects a pin-mux state for a device. On our target there is
 * no pinctrl controller (the DSI panel-backlight PWM mux it selects is embedded-only),
 * so it returns NULL — a non-error "no pinctrl" handle, which intel_dsi_vbt.c tolerates
 * (it only IS_ERR-checks the result). Compiled for completeness; never runs on eDP.
 */
#ifndef _LKPI_PINCTRL_CONSUMER_H
#define _LKPI_PINCTRL_CONSUMER_H
#include <linux/err.h>

struct device;
struct pinctrl;

static inline struct pinctrl *devm_pinctrl_get_select(struct device *dev, const char *name){
	(void)dev; (void)name; return NULL;
}
static inline struct pinctrl *devm_pinctrl_get_select_default(struct device *dev){
	(void)dev; return NULL;
}

#endif
