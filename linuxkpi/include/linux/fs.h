#ifndef _LKPI_FS_H
#define _LKPI_FS_H
#include <linux/types.h>
#include <linux/wait.h>
#include <linux/atomic.h>   /* struct file.f_count is an atomic_long_t (i915 shmem_utils bumps it) */
struct vm_area_struct;
struct inode { unsigned long i_ino; void *i_mapping; umode_t i_mode; void *i_private; };
struct file { void *private_data; void *f_mapping; unsigned int f_flags; loff_t f_pos; const struct file_operations *f_op; struct inode *f_inode; atomic_long_t f_count; };
struct file_operations {
  void *owner;
  int (*open)(struct inode *, struct file *);
  int (*release)(struct inode *, struct file *);
  void *read, *write;
  int (*mmap)(struct file *, struct vm_area_struct *);
  void *poll;
  void *unlocked_ioctl, *compat_ioctl, *llseek, *read_iter, *write_iter, *mmap_supported_flags;
  unsigned int fop_flags;
};
/* address_space doubles as the shmem page cache for gem_shmem: `pages` is a lazily
 * populated per-index array of single-page folios (see kpi_misc.c shmem_*). */
struct address_space { void *host; struct page **pages; unsigned long nrpages; unsigned gfp_mask; };
static inline loff_t i_size_read(const struct inode *i){ (void)i; return 0; }
extern loff_t noop_llseek(struct file *file, loff_t offset, int whence);
#define FOP_UNSIGNED_OFFSET (1u<<5)
static inline const struct file_operations *fops_get(const struct file_operations *f){ return f; }
static inline void fops_put(const struct file_operations *f){ (void)f; }
#define replace_fops(f, fops) do { (f)->f_op = (fops); } while (0)
static inline int register_chrdev(unsigned major, const char *name, const struct file_operations *fops){ (void)major;(void)name;(void)fops; return 0; }
static inline void unregister_chrdev(unsigned major, const char *name){ (void)major;(void)name; }
static inline int __register_chrdev(unsigned major, unsigned base, unsigned count, const char *name, const struct file_operations *fops){ (void)major;(void)base;(void)count;(void)name;(void)fops; return 0; }
static inline void __unregister_chrdev(unsigned major, unsigned base, unsigned count, const char *name){ (void)major;(void)base;(void)count;(void)name; }
#endif

#ifndef _LKPI_FS_EXTRA
#define _LKPI_FS_EXTRA
static inline struct inode *file_inode(struct file *f){ return f?f->f_inode:0; }
static inline unsigned iminor(struct inode *i){ (void)i; return 0; }
static inline unsigned imajor(struct inode *i){ (void)i; return 0; }
#define old_encode_dev(d) ((unsigned)(d))
struct file *anon_inode_getfile(const char *name, const struct file_operations *ops, void *priv, int flags);
int anon_inode_getfd(const char *name, const struct file_operations *ops, void *priv, int flags);
#endif

#ifndef _LKPI_FCNTL_X
#define _LKPI_FCNTL_X
#define O_NONBLOCK 04000
#define O_CLOEXEC  02000000
#define O_RDWR     2
#define O_EXCL     0200
#define O_CREAT    0100
static inline struct file *file_clone_open(struct file *f){ return f; }
#endif



#ifndef _LKPI_FS_PSEUDO
#define _LKPI_FS_PSEUDO
struct vfsmount { struct super_block *mnt_sb; };
struct super_block { void *s_fs_info; unsigned long s_blocksize; };
struct fs_context { int unused; };
struct file_system_type {
  const char *name; void *owner; int fs_flags;
  int (*init_fs_context)(struct fs_context *);
  struct dentry *(*mount)(struct file_system_type*, int, const char*, void*);
  void (*kill_sb)(struct super_block *);
};
static inline void kill_anon_super(struct super_block *sb){ (void)sb; }
static inline int init_pseudo(struct fs_context *fc, unsigned long magic){ (void)fc;(void)magic; return 0; }
/* get_fs_type: look up a registered filesystem by name. i915 uses it to find "tmpfs" for its gemfs
 * mount; NanOS has no tmpfs registry, so it returns NULL and i915 falls back to the default shmem. */
static inline struct file_system_type *get_fs_type(const char *name){ (void)name; return 0; }
static inline struct vfsmount *kern_mount(struct file_system_type *t){ (void)t; return 0; }
static inline void kern_unmount(struct vfsmount *m){ (void)m; }
static inline int simple_pin_fs(struct file_system_type *t, struct vfsmount **m, int *count){ (void)t;(void)m;(void)count; return 0; }
static inline void simple_release_fs(struct vfsmount **m, int *count){ (void)m;(void)count; }
static inline struct inode *alloc_anon_inode(struct super_block *sb){ (void)sb; return 0; }
static inline void iput(struct inode *i){ (void)i; }
#endif
