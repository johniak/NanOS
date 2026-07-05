#ifndef _LKPI_UUID_H
#define _LKPI_UUID_H
#include <linux/types.h>
#include <linux/string.h>
typedef struct { unsigned char b[16]; } uuid_le;
/* Length of a formatted UUID string "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" (no NUL), as Linux. i915
 * perf sizes its OA-config uuid[] buffer with it. */
#ifndef UUID_SIZE
#define UUID_SIZE 16   /* raw UUID/GUID length in bytes */
#endif
#ifndef UUID_STRING_LEN
#define UUID_STRING_LEN 36
static inline bool uuid_is_valid(const char *uuid){ if(!uuid) return false; for(int i=0;i<36;i++){ char c=uuid[i]; if(i==8||i==13||i==18||i==23){ if(c!='-') return false; } else if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) return false; } return uuid[36]==0; }
#endif
static inline void uuid_copy(uuid_t *d, const uuid_t *s){ memcpy(d,s,sizeof(*d)); }
static inline void import_uuid(uuid_t *d, const unsigned char *s){ memcpy(d->b,s,16); }
static inline void export_uuid(unsigned char *d, const uuid_t *s){ memcpy(d,s->b,16); }
static inline bool uuid_equal(const uuid_t *a, const uuid_t *b){ return memcmp(a,b,sizeof(*a))==0; }
/* guid_t is the little-endian sibling of uuid_t; drm_dp_mst uses it for MST GUIDs. Same 16-byte
 * blob here (byte-order only matters on the wire, which the DRM helpers handle themselves). */
static inline void guid_copy(guid_t *d, const guid_t *s){ memcpy(d,s,sizeof(*d)); }
static inline void import_guid(guid_t *d, const unsigned char *s){ memcpy(d->b,s,16); }
static inline void export_guid(unsigned char *d, const guid_t *s){ memcpy(d,s->b,16); }
static inline bool guid_equal(const guid_t *a, const guid_t *b){ return memcmp(a,b,sizeof(*a))==0; }
static inline bool guid_is_null(const guid_t *g){ guid_t z = {{0}}; return memcmp(g,&z,sizeof(z))==0; }
/* Generate a (pseudo-)unique GUID. drm_dp_mst tags each MST branch with one. MST is off the eDP
 * bring-up path; a monotone counter woven into the 16 bytes is unique-enough and deterministic. */
static inline void guid_gen(guid_t *g){
	static unsigned long ctr;
	unsigned long v = ++ctr;
	for (int i = 0; i < 16; i++) g->b[i] = (unsigned char)(v >> ((i & 7) * 8)) ^ (unsigned char)(i * 0x9e);
	g->b[6] = (g->b[6] & 0x0f) | 0x40;  /* version 4 */
	g->b[8] = (g->b[8] & 0x3f) | 0x80;  /* variant  */
}
#endif
