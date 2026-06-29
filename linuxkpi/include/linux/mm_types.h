#ifndef _LKPI_MM_TYPES_H
#define _LKPI_MM_TYPES_H
#include <linux/types.h>
#include <linux/rbtree.h>
struct vm_area_struct { unsigned long vm_start, vm_end; unsigned long vm_flags; pgprot_t vm_page_prot; void *vm_private_data; const struct vm_operations_struct *vm_ops; struct mm_struct *vm_mm; unsigned long vm_pgoff; struct file *vm_file; };
struct mm_struct { int n; };
struct folio;  /* single-page model: a folio pointer == a page (kernel virtual addr) */
struct vm_fault { struct vm_area_struct *vma; unsigned long address; unsigned long pgoff; unsigned long flags; void *page; };
struct vm_operations_struct { void (*open)(struct vm_area_struct*); void (*close)(struct vm_area_struct*); int (*fault)(struct vm_fault*); int (*access)(struct vm_area_struct*,unsigned long,void*,int,int); };
typedef unsigned long vm_fault_t;
#define VM_FAULT_NOPAGE 0x0100
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_OOM    0x0001
#define VM_SHARED   0x00000008
#define VM_MAYSHARE 0x00000080
#define VM_PFNMAP   0x00000400
#define VM_IO       0x00004000
#define VM_DONTEXPAND 0x00040000
#define VM_DONTDUMP 0x04000000
#define VM_MIXEDMAP 0x10000000
#endif
