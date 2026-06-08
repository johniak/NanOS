; greet_import.S — the import library for greet.ndl (the Windows-PE "import library"
; model). For each imported function it provides:
;   - a THUNK with the function's public name (`nx_greet: jmp [__imp_nx_greet]`), so a
;     program just calls nx_greet() and the linker resolves it here;
;   - an IAT slot (`__imp_nx_greet`), a function pointer the dynamic loader patches with
;     greet.ndl's exported address at load time;
;   - an import descriptor in .nximports — NxImport{ nameOff, slotAddr } — which mknx
;     lifts into the program's import table so the loader knows which name binds the slot.
; Hand-written here for two symbols; Stage 3 generates this for libc.ndl's whole API.
[BITS 32]

section .nximports
; NxImport[] = { unsigned nameOff; unsigned slotAddr; } per imported symbol.
    dd nm_nx_greet
    dd __imp_nx_greet
    dd nm_nx_greeting
    dd __imp_nx_greeting

section .data
[GLOBAL __imp_nx_greet]
[GLOBAL __imp_nx_greeting]
__imp_nx_greet:    dd 0
__imp_nx_greeting: dd 0

section .rodata
nm_nx_greet:    db "nx_greet", 0
nm_nx_greeting: db "nx_greeting", 0

section .text
[GLOBAL nx_greet]
[GLOBAL nx_greeting]
nx_greet:
    jmp [__imp_nx_greet]
nx_greeting:
    jmp [__imp_nx_greeting]
