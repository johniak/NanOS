/* linuxkpi/include/linux/of.h — Open Firmware / device-tree stubs.
 * NanOS/x86 has no device tree, so node refcounting is a no-op and lookups find nothing. */
#ifndef _LKPI_OF_H
#define _LKPI_OF_H

struct device_node;
static inline struct device_node *of_node_get(struct device_node *n){ return n; }
static inline void of_node_put(struct device_node *n){ (void)n; }
static inline bool of_node_is_available(const struct device_node *n){ (void)n; return false; }
static inline bool of_property_present(const struct device_node *n, const char *name){ (void)n; (void)name; return false; }
static inline bool of_property_read_bool(const struct device_node *n, const char *name){ (void)n; (void)name; return false; }
struct fwnode_handle;
static inline struct fwnode_handle *of_fwnode_handle(struct device_node *n){ (void)n; return 0; }
/* DT child iteration: no device tree, so the body never runs (child stays NULL). */
#define for_each_available_child_of_node(parent, child) \
	for ((child) = 0; (child); )
#define for_each_child_of_node(parent, child) for_each_available_child_of_node(parent, child)

#endif
