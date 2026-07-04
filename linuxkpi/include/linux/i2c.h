#ifndef _LKPI_I2C_H
#define _LKPI_I2C_H
#include <linux/device.h>
#include <linux/workqueue.h>   /* drm_dp_helper.h embeds work_struct/delayed_work and reaches the
                                * full definitions only through <linux/i2c.h> (mirrors upstream) */
struct i2c_adapter { char name[48]; void *algo_data; struct device dev; };
struct i2c_msg { unsigned short addr, flags, len; unsigned char *buf; };
struct i2c_client { struct i2c_adapter *adapter; };
#define I2C_M_RD 0x0001
static inline int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n){ (void)a;(void)m;(void)n; return -1; }
static inline void *i2c_get_adapdata(const struct i2c_adapter *a){ return a->algo_data; }
#endif
