; greet_import.S — the import library for greet.ndl (the Windows-PE "import library"
; model). For each imported function it provides:
;   - a THUNK with the function's public name (`nx_greet: jmp [__imp_nx_greet]`), so a
;     program just calls nx_greet() and the linker resolves it here;
;   - an IAT slot (`__imp_nx_greet`), a function pointer the dynamic loader patches with
;     greet.ndl's exported address at load time.
; mknx derives the program's import table from the global `__imp_<name>` slots, so no
; descriptor section is needed. Hand-written here; mknx --implib generates the same shape
; for libc.ndl's whole API.
[BITS 32]

section .data
[GLOBAL __imp_nx_greet]
[GLOBAL __imp_nx_greeting]
__imp_nx_greet:    dd 0
__imp_nx_greeting: dd 0

section .text
[GLOBAL nx_greet]
[GLOBAL nx_greeting]
nx_greet:
    jmp [__imp_nx_greet]
nx_greeting:
    jmp [__imp_nx_greeting]
