/* linuxkpi/include/linux/i2c-algo-bit.h — bit-banged I2C fallback for GMBUS. STUB (native GMBUS is
 * used on the Dell; the bit-bang fallback is rarely taken). */
#ifndef _LINUXKPI_LINUX_I2C_ALGO_BIT_H
#define _LINUXKPI_LINUX_I2C_ALGO_BIT_H
struct i2c_adapter;
struct i2c_algo_bit_data {
	void *data;
	void (*setsda)(void *, int); void (*setscl)(void *, int);
	int (*getsda)(void *); int (*getscl)(void *);
	int (*pre_xfer)(struct i2c_adapter *); void (*post_xfer)(struct i2c_adapter *);
	int udelay; int timeout;
	bool can_do_atomic;   /* may the bus be driven from atomic context? */
};
static inline int i2c_bit_add_bus(struct i2c_adapter *a){ (void)a; return 0; }
/* the generic bit-bang algorithm i915_gmbus falls back to; master_xfer/functionality fail (no bus). */
struct i2c_algorithm;
extern const struct i2c_algorithm i2c_bit_algo;
#endif
