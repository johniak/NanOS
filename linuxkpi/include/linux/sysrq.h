/* linuxkpi/include/linux/sysrq.h — magic-sysrq keys. NanOS registers none; hooks are no-ops. */
#ifndef _LINUXKPI_LINUX_SYSRQ_H
#define _LINUXKPI_LINUX_SYSRQ_H
struct sysrq_key_op { void *dummy; };
static inline int register_sysrq_key(int key, const struct sysrq_key_op *op){ (void)key;(void)op; return 0; }
static inline int unregister_sysrq_key(int key, const struct sysrq_key_op *op){ (void)key;(void)op; return 0; }
#endif
