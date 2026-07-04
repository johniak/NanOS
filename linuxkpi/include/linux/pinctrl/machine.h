/*
 * linuxkpi/include/linux/pinctrl/machine.h — pinctrl mapping-table entries.
 *
 * struct/enum/macro shapes verbatim from Linux 6.12 so intel_dsi_vbt.c's static
 * soc_pwm_pinctrl_map[] compiles. pinctrl_register_mappings is a no-op: the mapping
 * routes a SoC PWM pin group for an embedded DSI panel backlight — not present on the
 * Dell (eDP, PWM backlight driven by the display engine). Compiled, never executed.
 */
#ifndef _LKPI_PINCTRL_MACHINE_H
#define _LKPI_PINCTRL_MACHINE_H

enum pinctrl_map_type {
	PIN_MAP_TYPE_INVALID,
	PIN_MAP_TYPE_DUMMY_STATE,
	PIN_MAP_TYPE_MUX_GROUP,
	PIN_MAP_TYPE_CONFIGS_PIN,
	PIN_MAP_TYPE_CONFIGS_GROUP,
};

struct pinctrl_map_mux {
	const char *group;
	const char *function;
};

struct pinctrl_map_configs {
	const char *group_or_pin;
	unsigned long *configs;
	unsigned int num_configs;
};

struct pinctrl_map {
	const char *dev_name;
	const char *name;
	enum pinctrl_map_type type;
	const char *ctrl_dev_name;
	union {
		struct pinctrl_map_mux mux;
		struct pinctrl_map_configs configs;
	} data;
};

#define PIN_MAP_MUX_GROUP(dev, state, pinctrl, grp, func)	\
	{							\
		.dev_name = dev,				\
		.name = state,					\
		.type = PIN_MAP_TYPE_MUX_GROUP,			\
		.ctrl_dev_name = pinctrl,			\
		.data.mux = {					\
			.group = grp,				\
			.function = func,			\
		},						\
	}

static inline int pinctrl_register_mappings(const struct pinctrl_map *map,
					    unsigned int num_maps){ (void)map;(void)num_maps; return 0; }
static inline void pinctrl_unregister_mappings(const struct pinctrl_map *map){ (void)map; }

#endif
