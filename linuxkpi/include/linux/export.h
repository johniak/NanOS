/*
 * linuxkpi/include/linux/export.h — EXPORT_SYMBOL etc. are no-ops: the shim links the
 * driver+core into one module, so there is no inter-module symbol table.
 */
#ifndef _LINUXKPI_LINUX_EXPORT_H
#define _LINUXKPI_LINUX_EXPORT_H

#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_GPL_FOR_MODULES(sym, mods)
#define EXPORT_SYMBOL_NS(sym, ns)
#define EXPORT_SYMBOL_NS_GPL(sym, ns)
#define THIS_MODULE ((struct module *)0)
#define MODULE_IMPORT_NS(ns)

#endif /* _LINUXKPI_LINUX_EXPORT_H */
