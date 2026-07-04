#ifndef _LKPI_IOSYS_MAP_H
#define _LKPI_IOSYS_MAP_H
#include <linux/types.h>
#include <linux/string.h>
struct iosys_map { union { void *vaddr_iomem; void *vaddr; }; bool is_iomem; };
#define IOSYS_MAP_INIT_VADDR(v) { .vaddr = (v), .is_iomem = false }
#define IOSYS_MAP_INIT_OFFSET(m,off) ({ struct iosys_map c=*(m); c.vaddr=(char*)c.vaddr+(off); c; })
static inline void iosys_map_set_vaddr(struct iosys_map *m, void *vaddr){ m->vaddr=vaddr; m->is_iomem=false; }
static inline void iosys_map_set_vaddr_iomem(struct iosys_map *m, void *v){ m->vaddr_iomem=v; m->is_iomem=true; }
static inline bool iosys_map_is_null(const struct iosys_map *m){ return !m->vaddr; }
static inline bool iosys_map_is_set(const struct iosys_map *m){ return !!m->vaddr; }
static inline void iosys_map_clear(struct iosys_map *m){ m->vaddr=0; m->is_iomem=false; }
static inline void iosys_map_memcpy_to(struct iosys_map *d, size_t off, const void *s, size_t len){ memcpy((char*)d->vaddr+off, s, len); }
static inline void iosys_map_memcpy_from(void *dst, const struct iosys_map *s, size_t off, size_t len){ memcpy(dst, (char*)s->vaddr+off, len); }
static inline void iosys_map_memset(struct iosys_map *m, size_t off, int v, size_t len){ memset((char*)m->vaddr+off, v, len); }
static inline void iosys_map_incr(struct iosys_map *m, size_t incr){ m->vaddr=(char*)m->vaddr+incr; }
static inline void *iosys_map_rd_field_ptr(struct iosys_map*m){return m->vaddr;}
/* iosys_map_wr_field(map, offset, struct_type, field, val): write `val` into map at the byte offset
 * of `field` within `struct_type`. Flat vaddr map, so it's a direct store at vaddr+offset+offsetof. */
#define iosys_map_wr_field(map__, offset__, type__, field__, val__) \
	do { *(__typeof__(((type__ *)0)->field__) *)((char *)(map__)->vaddr + (offset__) + offsetof(type__, field__)) = (val__); } while (0)
#define iosys_map_rd_field(map__, offset__, type__, field__) \
	(*(__typeof__(((type__ *)0)->field__) *)((char *)(map__)->vaddr + (offset__) + offsetof(type__, field__)))
#endif
