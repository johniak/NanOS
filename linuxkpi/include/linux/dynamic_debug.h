#ifndef _LKPI_DYNAMIC_DEBUG_H
#define _LKPI_DYNAMIC_DEBUG_H
enum ddebug_class_type { DD_CLASS_TYPE_DISJOINT_BITS, DD_CLASS_TYPE_LEVEL_NUM, DD_CLASS_TYPE_DISJOINT_NAMES, DD_CLASS_TYPE_LEVEL_NAMES };
#define DECLARE_DYNDBG_CLASSMAP(var, type, base, ...) static const char *var[] __attribute__((unused)) = { __VA_ARGS__ }
#define DYNAMIC_DEBUG_BRANCH(d) 0
#define dynamic_pr_debug(fmt, ...) do{}while(0)
#endif
