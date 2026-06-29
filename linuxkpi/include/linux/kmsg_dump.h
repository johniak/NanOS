#ifndef _LKPI_KMSG_DUMP_H
#define _LKPI_KMSG_DUMP_H
enum kmsg_dump_reason { KMSG_DUMP_UNDEF=0, KMSG_DUMP_PANIC, KMSG_DUMP_OOPS };
struct kmsg_dumper { void (*dump)(struct kmsg_dumper*, enum kmsg_dump_reason); struct kmsg_dumper *next; };
static inline int kmsg_dump_register(struct kmsg_dumper *d){ (void)d; return 0; }
static inline int kmsg_dump_unregister(struct kmsg_dumper *d){ (void)d; return 0; }
#endif
