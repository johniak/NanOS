#ifndef _LKPI_I2C_H
#define _LKPI_I2C_H
#include <linux/device.h>
#include <linux/errno.h>       /* EAGAIN for the i2c_transfer retry loop */
#include <linux/workqueue.h>   /* drm_dp_helper.h embeds work_struct/delayed_work and reaches the
                                * full definitions only through <linux/i2c.h> (mirrors upstream) */
struct i2c_algorithm;
struct i2c_lock_operations;
struct i2c_adapter_quirks;
struct i2c_adapter { char name[48]; void *algo_data; const struct i2c_algorithm *algo; const struct i2c_adapter_quirks *quirks; const struct i2c_lock_operations *lock_ops; struct device dev; unsigned int class; int nr; int retries; int timeout; void *owner; struct module *owner_mod; };
struct i2c_lock_operations { void (*lock_bus)(struct i2c_adapter*, unsigned int); int (*trylock_bus)(struct i2c_adapter*, unsigned int); void (*unlock_bus)(struct i2c_adapter*, unsigned int); };
struct i2c_algorithm { int (*master_xfer)(struct i2c_adapter*, struct i2c_msg*, int); int (*master_xfer_atomic)(struct i2c_adapter*, struct i2c_msg*, int); unsigned int (*functionality)(struct i2c_adapter*); };
/* adapter timing/behaviour quirks (i2c-algo-bit publishes a no-clock-stretch quirk). */
#ifndef I2C_AQ_NO_CLK_STRETCH
#define I2C_AQ_NO_CLK_STRETCH  (1 << 4)
#endif
struct i2c_adapter_quirks { unsigned long flags; int max_num_msgs; unsigned short max_write_len, max_read_len, max_comb_1st_msg_len, max_comb_2nd_msg_len; };
struct i2c_msg { unsigned short addr, flags, len; unsigned char *buf; };
struct i2c_client { struct i2c_adapter *adapter; };
#define I2C_M_RD        0x0001
#define I2C_M_TEN       0x0010
#define I2C_M_STOP      0x8000
#define I2C_M_NOSTART   0x4000
#ifndef I2C_NAME_SIZE
#define I2C_NAME_SIZE 20
#define I2C_FUNC_I2C            0x00000001
#define I2C_FUNC_SMBUS_EMUL    0x0eff0008
#define I2C_FUNC_10BIT_ADDR    0x00000002
#define I2C_FUNC_NOSTART       0x00000010
#define I2C_FUNC_SMBUS_READ_BLOCK_DATA  0x01000000
#define I2C_FUNC_SMBUS_BLOCK_DATA       0x03000000
#define I2C_FUNC_SMBUS_BYTE_DATA        0x000c0000
#define I2C_FUNC_SMBUS_BLOCK_PROC_CALL  0x00008000
#define I2C_FUNC_SMBUS_PROC_CALL        0x00800000
#define I2C_FUNC_SMBUS_WRITE_BLOCK_DATA 0x02000000
#define I2C_FUNC_PROTOCOL_MANGLING      0x00000004
#define I2C_FUNC_SMBUS_PEC              0x00000008
#define I2C_FUNC_SMBUS_EMUL_ALL         0x8eff0008   /* EMUL | some block/proc-call bits (i2c-algo-bit) */
#define I2C_CLASS_DDC          (1<<3)
#endif
#define I2C_M_REV_DIR_ADDR 0x2000
#define I2C_M_IGNORE_NAK   0x1000
#define I2C_M_NO_RD_ACK    0x0800
#define I2C_M_RECV_LEN     0x0400
/* Real i2c_transfer: dispatch to the adapter's own algorithm, exactly like the Linux i2c core.
 * drm_dp_aux_init() registers aux->ddc with algo->master_xfer = drm_dp_i2c_xfer (I2C-over-AUX),
 * and intel_gmbus registers gmbus_xfer — so this is what makes DDC/EDID reads work over both the
 * eDP AUX channel (panel EDID -> fixed mode) and the gmbus HDMI/DVI ports. The old `return -1`
 * stub (virtio_gpu has no DDC panel) silently failed every drm_do_probe_ddc_edid(), which is why
 * the Dell eDP panel found no EDID and no fixed mode. Retry on -EAGAIN up to adapter->retries,
 * mirroring __i2c_transfer in drivers/i2c/i2c-core-base.c. */
static inline int __i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n){
	if (!a || !a->algo || !a->algo->master_xfer)
		return -1;
	int ret, tries = 0, retries = a->retries > 0 ? a->retries : 0;
	do {
		ret = a->algo->master_xfer(a, m, n);
	} while (ret == -EAGAIN && tries++ < retries);
	return ret;
}
static inline int i2c_transfer(struct i2c_adapter *a, struct i2c_msg *m, int n){
	return __i2c_transfer(a, m, n);
}
static inline void i2c_del_adapter(struct i2c_adapter *a){ (void)a; }
static inline int i2c_add_adapter(struct i2c_adapter *a){ (void)a; return 0; }
static inline int i2c_add_numbered_adapter(struct i2c_adapter *a){ (void)a; return 0; }
static inline void *i2c_get_adapdata(const struct i2c_adapter *a){ return a->algo_data; }
/* Look up a bus by number. No system-wide i2c adapter registry exists in the shim (gmbus
 * owns its adapter directly), so this returns NULL ("bus not found"); intel_dsi_vbt.c's
 * DSI-over-i2c panel path (embedded panels only) handles NULL by skipping. */
static inline struct i2c_adapter *i2c_get_adapter(int nr){ (void)nr; return 0; }
static inline void i2c_put_adapter(struct i2c_adapter *a){ (void)a; }
#ifndef I2C_SMBUS_BLOCK_MAX
#define I2C_SMBUS_BLOCK_MAX 32   /* max bytes in an SMBus block xfer (i2c-algo-bit) */
#endif
/* 8-bit address byte (7-bit addr << 1 | R/W) for a message; i2c-algo-bit shifts it onto the wire. */
static inline u8 i2c_8bit_addr_from_msg(const struct i2c_msg *msg){ return (u8)((msg->addr << 1) | ((msg->flags & I2C_M_RD) ? 1 : 0)); }
#endif
