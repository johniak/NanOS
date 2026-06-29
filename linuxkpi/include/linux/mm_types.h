#ifndef _LKPI_MM_TYPES_H
#define _LKPI_MM_TYPES_H
#include <linux/types.h>
#include <linux/rbtree.h>
struct vm_area_struct { unsigned long vm_start, vm_end; unsigned long vm_flags; void *vm_private_data; void *vm_ops; struct mm_struct *vm_mm; unsigned long vm_pgoff; };
struct mm_struct { int n; };
struct vm_fault { void *vma; unsigned long address; unsigned long pgoff; };
struct vm_operations_struct { void *fault; void *open; void *close; };
typedef unsigned long vm_fault_t;
#define VM_FAULT_NOPAGE 0x0100
#define VM_FAULT_SIGBUS 0x0002
#define VM_FAULT_OOM    0x0001
#endif
