#ifndef _LKPI_I2C_H
#define _LKPI_I2C_H
#include <linux/device.h>
#include <linux/workqueue.h>   /* drm_dp_helper.h embeds work_struct/delayed_work and reaches the
                                * full definitions only through <linux/i2c.h> (mirrors upstream) */
struct i2c_algorithm;
struct i2c_adapter { char name[48]; void *algo_data; const struct i2c_algorithm *algo; struct device dev; unsigned int class; int nr; int retries; int timeout; void *owner; struct module *owner_mod; };
struct i2c_algorithm { int (*master_xfer)(struct i2c_adapter*, struct i2c_msg*, int); unsigned int (*functionality)(struct i2c_adapter*); };
struct i2c_msg { unsigned short addr, flags, len; unsigned char *buf; };
struct i2c_client { struct i2c_adapter *adapter; };
#define I2C_M_RD        0x0001
#define I2C_M_TEN       0x0010
#define I2C_M_STOP      0x8000
#define I2C_M_NOSTART   0x4000
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_IGNORE_NAK   0x1000
#define I2C_M_NO_RD_ACK    0x0800
#define I2C_M_RECV_LEN     0x0400
static inline int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n){ (void)a;(void)m;(void)n; return -1; }
/* __i2c_transfer is the unlocked form; the shim's adapter has no real bus, so both fail alike. */
static inline int __i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n){ (void)a;(void)m;(void)n; return -1; }
static inline void i2c_del_adapter(struct i2c_adapter *a){ (void)a; }
static inline int i2c_add_adapter(struct i2c_adapter *a){ (void)a; return 0; }
static inline void *i2c_get_adapdata(const struct i2c_adapter *a){ return a->algo_data; }
#endif
