# NanOS Freestanding Library (`lib/` + `include/`)

The kernel is built `-ffreestanding -nostdlib -nostdinc++` — there is no libc and no STL. `lib/`
(plus `include/string.h`) is the small in-house utility layer that fills the gap: the freestanding
string/memory functions, two container classes (`String`, `List`), and the C++ ABI glue that lets
the compiler emit C++ at all. (Userland gets a *real* C library via `libc.ndl` instead —
@docs/nxe-ndl.md, @docs/writing-apps.md.)

---

## 1. Freestanding string/memory (`include/string.h` + `lib/string_funcs.cpp`)

The freestanding `<string.h>` declares just what the kernel uses — `memcpy`, `memmove`, `memset`,
`strlen`, `strcmp`, `strstr` — implemented in `lib/string_funcs.cpp`. The kernel include path
(`KINCLUDES`) has `-Iinclude` so `#include <string.h>` resolves to **this** header; the host test
build (`HINCLUDES`) omits `-Iinclude` so the same `<string.h>` resolves to **libc** and the tests
reuse the platform's functions (CLAUDE.md, Testing).

> macOS case-insensitivity trap: with `-Ilib` on the path, `#include <string.h>` would otherwise
> resolve to `lib/String.h` (the C++ class) → infinite include recursion. This is why the kernel
> compiles from a copy on the container's case-sensitive filesystem (CLAUDE.md).

---

## 2. `String` (`lib/String.h`)

A small **value-semantics** string. Each `String` owns its `char*` buffer: the copy ctor and
`operator=` deep-copy, and the destructor frees. That matters because the kernel heap's `free` is
real now (@docs/memory.md §5) and `String` is passed by value through many call sites — shared
buffers would double-free / use-after-free.

- ctors from `const char*` and from `int` (via `itoa`);
- `append` (uses `realloc`, which absorbs the old buffer — no separate `free`, which used to be a
  double-free); `operator+` for `String`/`char*`/`int`;
- `indexOf`/`startsWith` (over `strstr`), `compareTo` (`strcmp`), `getLenght`, `operator[]`,
  `operator char*` (decay to the raw buffer).
- `split`/`substring` were **removed** — they returned `List<String>`/`String` by value through a
  container that can't safely hold value types; the only user (ext path lookup) now parses
  components in place (@docs/filesystem.md).

`String` is the path/string currency of the VFS and the ext driver.

---

## 3. `List<T>` (`lib/List.h`)

A template **dynamic array** (grow-by-`capacityInc`, default 10): `add`/`insert`/`removeAt`/
`operator[]`/`getCount`. It uses the **`malloc`/`realloc`/`free` family consistently** — *not*
`new[]` — because `new[]` for a non-trivial `T` adds an array cookie that `realloc`/`free` don't
understand (the original `new[]`+`free` mismatch, plus a double-free in growth, were both bugs fixed
here). `increaseCapacity` relies on `realloc` to move/free the old block.

> Caveat: `List<T>` copies elements with `memcpy` (no element copy ctor), so it holds POD / trivially
> copyable types well, but not types whose copy needs a real constructor — which is exactly why
> `String::split` returning `List<String>` was removed.

---

## 4. C++ ABI glue (`lib/icxxabi.*`)

Itanium C++ ABI stubs so the freestanding C++ links: `__cxa_atexit` / `__cxa_finalize` (+
`__dso_handle`) over a 128-entry atexit table. The compiler emits `__cxa_atexit` calls to register
destructors for static-duration objects; these symbols must exist for the kernel to link, even
though **global constructors are not run** (the loader jumps straight to `kmain`, no `.ctors` pass —
CLAUDE.md). So in practice the table is the link-time contract, not a runtime teardown path (the
kernel never "exits").

---

## 5. Allocation backing

`String`/`List` and everything else allocate through `mm/memory_manager.*` → the kernel `Heap`
(@docs/memory.md §5). `memory_manager` declares `malloc`/`free` with **C++ linkage** (not
`extern "C"`); the host tests provide matching shims forwarding to libc builtins (CLAUDE.md,
Testing).

**Key files:** `include/string.h`, `lib/{string_funcs.cpp, String.h, String.cpp, List.h,
List.cpp, icxxabi.h, icxxabi.cpp}`, `mm/memory_manager.*`.
