/*
 * DynLoader.h
 *
 * The dynamic linker: loads an executable together with the shared libraries (.ndl) it
 * needs, mapping each module into the process address space at an assigned base,
 * relocating it, and resolving imports BY NAME against a flat symbol table built from
 * every loaded module's exports (the Windows-PE model — see kernel/NxFormat.h).
 *
 * The pure symbol-table logic (SymTable) is host-testable and lives inline here; the
 * orchestration (dynLoadProgram) is kernel-only glue over <arch/...> + the VFS, like
 * kernel/Exec.cpp.
 */
#pragma once

namespace arch { struct AddressSpace; }

namespace kernel {

class Vfs;

// A flat name -> address table. Names are copied into an internal pool so the source
// bytes (which live in a load buffer that gets reused/zeroed) need not outlive add().
// Self-contained (no malloc/libc) so it is trivially host-testable.
class SymTable {
public:
	SymTable() : m_poolUsed(0), m_count(0) {}

	// Copy `name` and record its address. Returns false if the table or name pool is full,
	// or the name is already present (first definition wins, flat-namespace).
	bool add(const char* name, unsigned addr) {
		if (find(name))
			return false;
		int len = 0;
		while (name[len])
			len++;
		len++;   // NUL
		if (m_count >= MAX || m_poolUsed + len > POOL)
			return false;
		char* dst = m_pool + m_poolUsed;
		for (int i = 0; i < len; i++)
			dst[i] = name[i];
		m_poolUsed += len;
		m_names[m_count] = dst;
		m_addrs[m_count] = addr;
		m_count++;
		return true;
	}

	// Address for `name`, or 0 if absent.
	unsigned find(const char* name) const {
		for (int i = 0; i < m_count; i++) {
			const char* a = m_names[i];
			const char* b = name;
			while (*a && *a == *b) { a++; b++; }
			if (*a == *b)
				return m_addrs[i];
		}
		return 0;
	}

	int count() const { return m_count; }

private:
	// Sized to hold a whole C library's export set with headroom. libc.ndl exports the full
	// picolibc surface (~1300 symbols, ~14 KB of names) so a hosted port like bash resolves
	// every libc reference; the previous 512/16 KB caps silently dropped exports past the
	// limit (add() returns false), which left late-registered symbols like execve/exit
	// unresolvable and PID 1 failing to load. ~50 KB per table, heap-allocated per needed lib.
	static const int MAX = 2048;
	static const int POOL = 32768;
	char m_pool[POOL];
	int m_poolUsed;
	const char* m_names[MAX];
	unsigned m_addrs[MAX];
	int m_count;
};

// Load `exeImage` (already staged at its load base in `exeCap` bytes of buffer) as a
// dynamically-linked program: read its needed-library list, load each .ndl from
// /disks/main/nanos/lib into `space`, build the flat symbol table from their exports,
// then bind the executable's imports against it. Returns 0 and *entryOut (the relocated
// entry) on success, <0 on error. Caller maps the executable image itself (archLoadUser).
int dynLoadProgram(Vfs* vfs, void* exeImage, unsigned exeCap,
		arch::AddressSpace* space, unsigned* entryOut);

}
