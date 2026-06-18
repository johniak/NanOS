/* clone64.s — x86_64 __clone for the vendored musl pthread core (mirrors musl
 * src/thread/x86_64/clone.s). The i386 counterpart is clone.s; the Makefile picks this
 * file when ARCH=x86_64. SysV-AMD64 args: rdi=func rsi=stack rdx=flags rcx=arg r8=ptid
 * r9=tls, 8(%rsp)=ctid. Uses the `syscall` instruction; SYS_clone=56 / SYS_exit=60 are the
 * NanOS x86_64 numbers (kernel/SyscallNr.h). */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	xor %eax,%eax
	mov $56,%al
	mov %rdi,%r11
	mov %rdx,%rdi
	mov %r8,%rdx
	mov %r9,%r8
	mov 8(%rsp),%r10
	mov %r11,%r9
	and $-16,%rsi
	sub $8,%rsi
	mov %rcx,(%rsi)
	syscall
	test %eax,%eax
	jnz 1f
	xor %ebp,%ebp
	pop %rdi
	call *%r9
	mov %rax,%rdi
	xor %eax,%eax
	mov $60,%al
	syscall
	hlt
1:	ret
