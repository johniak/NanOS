/* dlfcn.h — NanOS has no runtime shared-object loading. The dl* family is provided so ports that
 * optionally dlopen() a plugin (htop's SystemdMeter probing libsystemd.so.0) compile and link;
 * dlopen() always fails, so those features degrade to "unavailable" at runtime. See posixstubs.c. */
#ifndef _NANOS_DLFCN_H
#define _NANOS_DLFCN_H

/* extern "C" so C++ ports (node/V8 native-addon glue) reference the unmangled dl* symbols libc-glue
 * defines with C linkage — without this the C++ call mangles to _Z6dlopen... and links to address 0. */
#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100
/* Pseudo-handles for dlsym: search the default global scope / the next object. NanOS has no runtime
 * loader (dlsym returns 0), so these are only used as the handle argument. */
#define RTLD_DEFAULT ((void*) 0)
#define RTLD_NEXT    ((void*) -1)

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int   dlclose(void* handle);
char* dlerror(void);

/* dladdr: map an address to its object/symbol. NanOS has no such runtime table, so the stub
 * returns 0 (not found) — Mesa's build_id.c then finds no build-id (harmless; cache disabled). */
typedef struct {
	const char* dli_fname;   /* object path */
	void*       dli_fbase;   /* object base address */
	const char* dli_sname;   /* nearest symbol name */
	void*       dli_saddr;   /* nearest symbol address */
} Dl_info;
int dladdr(const void* addr, Dl_info* info);

#ifdef __cplusplus
}
#endif

#endif
