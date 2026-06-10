/*
 * kext_rt.cpp — minimal C/C++ runtime linked into every nkext.
 *
 * The loader does NOT link libc/libstdc++ into a module, so a kext must carry the few
 * symbols the compiler emits (memset/memcpy) and that C++ needs (operator new/delete over
 * the kernel allocator, __cxa_pure_virtual). Allocation is delegated to the kernel via the
 * imported knx_malloc/knx_free.
 */
#include <stddef.h>

extern "C" void* knx_malloc(unsigned);
extern "C" void  knx_free(void*);

extern "C" void* memset(void* d, int c, size_t n) {
	unsigned char* p = (unsigned char*) d;
	for (size_t i = 0; i < n; i++)
		p[i] = (unsigned char) c;
	return d;
}
extern "C" void* memcpy(void* d, const void* s, size_t n) {
	unsigned char* a = (unsigned char*) d;
	const unsigned char* b = (const unsigned char*) s;
	for (size_t i = 0; i < n; i++)
		a[i] = b[i];
	return d;
}

void* operator new(size_t sz)        { return knx_malloc((unsigned) sz); }
void* operator new[](size_t sz)      { return knx_malloc((unsigned) sz); }
void  operator delete(void* p)       { knx_free(p); }
void  operator delete[](void* p)     { knx_free(p); }

extern "C" void __cxa_pure_virtual() { for (;;) {} }
