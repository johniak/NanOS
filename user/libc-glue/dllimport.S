; dllimport.S — IAT slots for libc.ndl's DATA symbols (the Windows-PE imported-data model).
;
; A program can't call a data symbol, so the function-thunk trick doesn't apply: it must
; access the datum through a pointer the loader fills in. These `__imp_<name>` slots are
; that pointer (the loader writes &<name>-in-libc.ndl into each). nx-dllimport.h then
; redefines stdout/stderr/stdin/errno to `(*__imp_<name>)`, so a normal `fputs(s, stdout)`
; dereferences the slot. The slots live in `.nxlib.libc.ndl` so mknx tags these imports as
; coming from libc.ndl (per-DLL namespace), exactly like the function thunks.
[BITS 32]

section .nxlib.libc.ndl progbits alloc write align=4
[GLOBAL __imp_stdin]
[GLOBAL __imp_stdout]
[GLOBAL __imp_stderr]
[GLOBAL __imp_errno]
__imp_stdin:  dd 0
__imp_stdout: dd 0
__imp_stderr: dd 0
__imp_errno:  dd 0
