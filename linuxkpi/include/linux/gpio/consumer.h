/* linuxkpi/include/linux/gpio/consumer.h — GPIO descriptors (DSI panel reset/enable). STUB. */
#ifndef _LINUXKPI_LINUX_GPIO_CONSUMER_H
#define _LINUXKPI_LINUX_GPIO_CONSUMER_H
#include <linux/types.h>
struct device; struct gpio_desc;
enum gpiod_flags { GPIOD_ASIS = 0, GPIOD_IN, GPIOD_OUT_LOW, GPIOD_OUT_HIGH };
static inline struct gpio_desc *devm_gpiod_get(struct device *dev, const char *con_id, enum gpiod_flags f){ (void)dev;(void)con_id;(void)f; return 0; }
static inline struct gpio_desc *devm_gpiod_get_index(struct device *dev, const char *con_id, unsigned idx, enum gpiod_flags f){ (void)dev;(void)con_id;(void)idx;(void)f; return 0; }
static inline struct gpio_desc *devm_gpiod_get_optional(struct device *dev, const char *con_id, enum gpiod_flags f){ (void)dev;(void)con_id;(void)f; return 0; }
static inline void gpiod_set_value(struct gpio_desc *d, int v){ (void)d;(void)v; }
static inline void gpiod_set_value_cansleep(struct gpio_desc *d, int v){ (void)d;(void)v; }
static inline int gpiod_get_value(const struct gpio_desc *d){ (void)d; return 0; }
static inline void gpiod_put(struct gpio_desc *d){ (void)d; }
#endif
