#ifndef _LKPI_MODULEPARAM_H
#define _LKPI_MODULEPARAM_H
#define module_param(a,b,c)
#define module_param_named(a,b,c,d)
#define module_param_unsafe(a,b,c)
#define module_param_named_unsafe(a,b,c,d)
#define MODULE_PARM_DESC(a,b)
#define core_param(a,b,c,d)
/* reference ops+arg so the driver's static param-callback table isn't flagged -Wunused. */
#define module_param_cb(name,ops,arg,perm) \
	__attribute__((unused)) static const void *__lkpi_kp_##name[] = { (const void *)(ops), (const void *)(arg) }
#define module_param_cb_unsafe(name,ops,arg,perm) module_param_cb(name,ops,arg,perm)
#define kernel_param_lock(m) do{}while(0)
#define kernel_param_unlock(m) do{}while(0)
struct kernel_param;
/* param get/set callback table (i915_mitigations/i915_params register these). */
struct kernel_param_ops {
	unsigned int flags;
	int (*set)(const char *val, const struct kernel_param *kp);
	int (*get)(char *buffer, const struct kernel_param *kp);
	void (*free)(void *arg);
};
struct kernel_param {
	const char *name;
	const struct kernel_param_ops *ops;
	unsigned short perm;
	void *arg;
};
#endif
