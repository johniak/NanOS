/*
 * ThreadArea.h — the set_thread_area(2) user_desc helper (pure, MI, host-tested).
 *
 * Linux i386 set_thread_area takes a `struct user_desc`. NanOS has a SINGLE fixed TLS
 * GDT descriptor (entry 6, selector 0x33; see arch::archLoadThreadTls / Gdt::setTlsBase),
 * so set_thread_area never allocates a slot — it just points entry 6 at the caller's TLS
 * block. The pure part (decode the struct, return the fixed selector, write the entry
 * number back so musl can compute %gs) lives here, free of any arch detail.
 */
#ifndef THREADAREA_H_
#define THREADAREA_H_

namespace kernel {

// Linux i386 struct user_desc. The first three fields are full words; `flags` packs the
// seg_32bit/contents/read_exec_only/limit_in_pages/seg_not_present/useable bitfields. We
// only consume entry_number (write the fixed slot back) and base_addr (the TLS block VA).
struct UserDesc {
	unsigned entry_number;
	unsigned base_addr;
	unsigned limit;
	unsigned flags;
};

// The fixed TLS GDT entry / selector NanOS reloads per thread.
enum { TLS_ENTRY = 6, TLS_SELECTOR = (TLS_ENTRY << 3) | 3 /* == 0x33, RPL 3 */ };

// Assign the fixed TLS slot to this user_desc: write entry_number back (musl reads it to
// derive %gs = (entry<<3)|3) and return the selector. Pure — no arch state touched here;
// the kernel glue separately records tlsBase and calls arch::archLoadThreadTls(base_addr).
inline int userDescToSelector(UserDesc* ud) {
	// Precondition: ud is non-null (the syscall dispatch validates the user pointer and
	// returns -EINVAL before calling here). Keeping this pure — no null branch returning a
	// bogus "success" selector — so the postcondition (entry_number written) always holds.
	ud->entry_number = TLS_ENTRY;
	return TLS_SELECTOR;
}

}

#endif /* THREADAREA_H_ */
