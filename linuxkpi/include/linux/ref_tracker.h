/* linuxkpi/include/linux/ref_tracker.h — reference leak tracker (debug). Disabled on NanOS: the dir
 * and cookies are inert. intel_wakeref's own refcount still works; only the leak-diagnostics are off. */
#ifndef _LINUXKPI_LINUX_REF_TRACKER_H
#define _LINUXKPI_LINUX_REF_TRACKER_H
#include <linux/types.h>
struct ref_tracker;
struct ref_tracker_dir { int dummy; };
static inline void ref_tracker_dir_init(struct ref_tracker_dir *dir, unsigned int quarantine, const char *name)
{ (void)dir;(void)quarantine;(void)name; }
static inline void ref_tracker_dir_exit(struct ref_tracker_dir *dir){ (void)dir; }
static inline void ref_tracker_dir_print(struct ref_tracker_dir *dir, unsigned int display_limit){ (void)dir;(void)display_limit; }
static inline int ref_tracker_dir_snprint(struct ref_tracker_dir *dir, char *buf, unsigned long size){ (void)dir; if(size) buf[0]=0; return 0; }
static inline int ref_tracker_alloc(struct ref_tracker_dir *dir, struct ref_tracker **tr, gfp_t gfp)
{ (void)dir;(void)tr;(void)gfp; return 0; }
static inline int ref_tracker_free(struct ref_tracker_dir *dir, struct ref_tracker **tr){ (void)dir;(void)tr; return 0; }
#endif
