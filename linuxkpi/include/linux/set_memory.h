#ifndef _LKPI_SET_MEMORY_H
#define _LKPI_SET_MEMORY_H
static inline void clflush(volatile void *p){ __asm__ __volatile__("clflush (%0)"::"r"(p):"memory"); }
static inline void clflushopt(volatile void *p){ clflush(p); }
static inline int set_pages_wb(struct page *p, int n){ (void)p;(void)n; return 0; }
static inline int set_pages_uc(struct page *p, int n){ (void)p;(void)n; return 0; }
static inline int set_memory_wc(unsigned long a, int n){ (void)a;(void)n; return 0; }
static inline int set_memory_wb(unsigned long a, int n){ (void)a;(void)n; return 0; }
static inline void wbinvd_on_all_cpus(void){ __asm__ __volatile__("wbinvd":::"memory"); }
extern struct resource iomem_resource;
#endif

#ifndef _LKPI_SET_PAGES_ARRAY
#define _LKPI_SET_PAGES_ARRAY
struct page;
static inline int set_pages_array_wc(struct page **p, int n){ (void)p;(void)n; return 0; }
static inline int set_pages_array_wb(struct page **p, int n){ (void)p;(void)n; return 0; }
static inline int set_pages_array_uc(struct page **p, int n){ (void)p;(void)n; return 0; }
#endif
