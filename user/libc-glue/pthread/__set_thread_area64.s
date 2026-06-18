/* __set_thread_area64.s — x86_64 thread-pointer install for the vendored musl pthread core
 * (mirrors musl src/thread/x86_64/__set_thread_area.s). The i386 counterpart is
 * __set_thread_area.s (GDT TLS via set_thread_area(2) + %gs); x86_64 installs %fs.base via
 * arch_prctl(ARCH_SET_FS=0x1002). SYS_arch_prctl=158 is the NanOS x86_64 number
 * (kernel/SyscallNr.h). The Makefile picks this file when ARCH=x86_64. */
.text
.global __set_thread_area
.hidden __set_thread_area
.type   __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rsi           /* arg2 = the new TLS base */
	movl $0x1002,%edi       /* arg1 = ARCH_SET_FS */
	movl $158,%eax          /* arch_prctl */
	syscall
	ret
