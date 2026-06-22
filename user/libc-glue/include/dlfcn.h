/* dlfcn.h — NanOS has no runtime shared-object loading. The dl* family is provided so ports that
 * optionally dlopen() a plugin (htop's SystemdMeter probing libsystemd.so.0) compile and link;
 * dlopen() always fails, so those features degrade to "unavailable" at runtime. See posixstubs.c. */
#ifndef _NANOS_DLFCN_H
#define _NANOS_DLFCN_H

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100

void* dlopen(const char* file, int mode);
void* dlsym(void* handle, const char* name);
int   dlclose(void* handle);
char* dlerror(void);

#endif
