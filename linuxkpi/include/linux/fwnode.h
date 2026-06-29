#ifndef _LKPI_FWNODE_H
#define _LKPI_FWNODE_H
struct fwnode_handle { int unused; };
static inline void fwnode_handle_put(struct fwnode_handle *f){ (void)f; }
static inline struct fwnode_handle *fwnode_handle_get(struct fwnode_handle *f){ return f; }
#endif
