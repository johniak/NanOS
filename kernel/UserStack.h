/*
 * UserStack.h — build a SysV-i386 argv+envp image at the top of a user stack.
 *
 * Layout (high → low): the argv then envp strings (NUL-terminated); then a 16-byte-aligned
 * block holding argc, the argc argv[] pointers + NULL, then the envc envp[] pointers + NULL.
 * esp ends pointing at argc (16-aligned), so crt0 reads argc = [esp], argv = esp + 4,
 * envp = esp + 4 + (argc + 1) * 4.
 *
 * Writing is delegated (write(va, src, len)) so the same code serves the kernel (write into
 * the child's mapped frames by physical address) and host tests (write into a buffer).
 */
#ifndef USERSTACK_H_
#define USERSTACK_H_

#include <stdint.h>

namespace kernel {

template <class WriteFn>
uint32_t buildUserStack(uint32_t userTop, const char* const* argv, int argc,
		const char* const* envp, int envc, WriteFn write) {
	uint32_t sp = userTop;
	uint32_t aptr[64], eptr[64];

	// 1) Copy the env strings, then the argv strings, top-down; record each one's user VA.
	for (int i = envc - 1; i >= 0; i--) {
		unsigned len = 0;
		while (envp[i][len]) len++;
		len++;
		sp -= len;
		write(sp, envp[i], len);
		eptr[i] = sp;
	}
	for (int i = argc - 1; i >= 0; i--) {
		unsigned len = 0;
		while (argv[i][len]) len++;
		len++;                              // include the NUL
		sp -= len;
		write(sp, argv[i], len);
		aptr[i] = sp;
	}

	// 2) Reserve the pointer block (argc + argc argv ptrs + NULL + envc env ptrs + NULL) and
	//    16-align its base, which becomes esp (pointing at argc). Keeps esp 16-aligned as the
	//    i386 ABI expects at program entry.
	uint32_t blockBytes = 4u * (uint32_t) (1 + (argc + 1) + (envc + 1));
	sp -= blockBytes;
	sp &= ~0xFu;

	uint32_t base = sp;
	uint32_t off = base;
	write(off, &argc, 4); off += 4;                       // argc
	for (int i = 0; i < argc; i++) { write(off, &aptr[i], 4); off += 4; }
	uint32_t nullp = 0;
	write(off, &nullp, 4); off += 4;                      // argv[argc] = NULL
	for (int i = 0; i < envc; i++) { write(off, &eptr[i], 4); off += 4; }
	write(off, &nullp, 4);                                // envp[envc] = NULL

	return base;
}

}  // namespace kernel

#endif /* USERSTACK_H_ */
