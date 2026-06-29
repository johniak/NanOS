#ifndef _LKPI_FS_H
#define _LKPI_FS_H
#include <linux/types.h>
#include <linux/wait.h>
struct vm_area_struct;
struct inode { unsigned long i_ino; void *i_mapping; umode_t i_mode; };
struct file { void *private_data; void *f_mapping; unsigned int f_flags; loff_t f_pos; const struct file_operations *f_op; struct inode *f_inode; };
struct file_operations {
  void *owner; void *open, *release, *read, *write;
  int (*mmap)(struct file *, struct vm_area_struct *);
  void *poll;
  void *unlocked_ioctl, *compat_ioctl, *llseek, *read_iter, *write_iter, *mmap_supported_flags;
  unsigned int fop_flags;
};
struct address_space { void *host; };
static inline loff_t i_size_read(const struct inode *i){ (void)i; return 0; }
extern loff_t noop_llseek(struct file *file, loff_t offset, int whence);
#define FOP_UNSIGNED_OFFSET (1u<<5)
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
static inline struct file *file_clone_open(struct file *f){ return f; }
#endif



#ifndef _LKPI_FS_PSEUDO
#define _LKPI_FS_PSEUDO
struct vfsmount { int unused; };
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
static inline int simple_pin_fs(struct file_system_type *t, struct vfsmount **m, int *count){ (void)t;(void)m;(void)count; return 0; }
static inline void simple_release_fs(struct vfsmount **m, int *count){ (void)m;(void)count; }
static inline struct inode *alloc_anon_inode(struct super_block *sb){ (void)sb; return 0; }
static inline void iput(struct inode *i){ (void)i; }
#endif
