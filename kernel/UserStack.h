/*
 * UserStack.h — build a SysV-i386 argv image at the top of a user stack.
 *
 * Layout (high → low): the argv strings (NUL-terminated); then a 16-byte-aligned
 * block holding argc, the argc argv[] pointers, and a NULL terminator. esp ends
 * pointing at argc (16-aligned), so crt0 reads argc = [esp], argv = esp + 4.
 *
 * Writing is delegated (write(va, src, len)) so the same code serves the kernel
 * (write into the child's mapped frames by physical address) and host tests
 * (write into a buffer).
 */
#ifndef USERSTACK_H_
#define USERSTACK_H_

#include <stdint.h>

namespace kernel {

template <class WriteFn>
uint32_t buildUserStack(uint32_t userTop, const char* const* argv, int argc, WriteFn write) {
	uint32_t sp = userTop;
	uint32_t ptrs[64];

	// 1) Copy the strings, top-down; record each one's user VA.
	for (int i = argc - 1; i >= 0; i--) {
		unsigned len = 0;
		while (argv[i][len]) len++;
		len++;                              // include the NUL
		sp -= len;
		write(sp, argv[i], len);
		ptrs[i] = sp;
	}

	// 2) Reserve the pointer block (argc + argc pointers + NULL) and 16-align its
	//    base, which becomes esp (pointing at argc). This keeps esp 16-aligned as
	//    the i386 ABI expects at program entry.
	uint32_t blockBytes = 4u * (uint32_t) (argc + 2);
	sp -= blockBytes;
	sp &= ~0xFu;

	uint32_t base = sp;
	write(base, &argc, 4);                  // argc
	for (int i = 0; i < argc; i++)
		write(base + 4u + 4u * (uint32_t) i, &ptrs[i], 4);
	uint32_t nullp = 0;
	write(base + 4u + 4u * (uint32_t) argc, &nullp, 4);   // argv[argc] = NULL

	return base;
}

}  // namespace kernel

#endif /* USERSTACK_H_ */
