/*
 * mknx — turn a linked i686 ELF (built with `ld --emit-relocs`) into a NanOS module
 * (.nxe executable or .ndl shared library). Replaces `objcopy -O binary`: besides the
 * flat load image it also emits the base-relocation table (R_386_32 fixup sites), the
 * export table (named symbols a .ndl provides), the import table (from a .nximports
 * section), and the needed-library list — so modules can load at any base and resolve
 * imports/exports by name (the Windows-PE model). See kernel/NxFormat.h.
 *
 * Build-host tool (native cc); parses ELF32 by hand (no libelf dependency).
 *
 * Usage:
 *   mknx <in.elf> <out.nxe|out.ndl> [--dll] [--export NAME]... [--export-all]
 *        [--need NAME]...
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "NxFormat.h"   /* compiled with -Ikernel */

/* ---- minimal ELF32 ---- */
typedef struct { unsigned char e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version,
	e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum,
	e_shentsize, e_shnum, e_shstrndx; } Elf32_Ehdr;
typedef struct { uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
	sh_link, sh_info, sh_addralign, sh_entsize; } Elf32_Shdr;
typedef struct { uint32_t st_name, st_value, st_size; unsigned char st_info, st_other;
	uint16_t st_shndx; } Elf32_Sym;
typedef struct { uint32_t r_offset, r_info; } Elf32_Rel;

#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_REL      9
#define SHT_NOBITS   8
#define SHF_ALLOC    2
#define R_386_32     1
#define EM_386       3
#define STB_GLOBAL   1
#define ELF32_R_TYPE(i) ((i) & 0xff)
#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)

static unsigned char* g_elf;
static long g_elfLen;
static Elf32_Ehdr* g_eh;
static Elf32_Shdr* g_sh;     /* section headers */
static int g_nsh;

static void die(const char* m) { fprintf(stderr, "mknx: %s\n", m); exit(1); }

static Elf32_Shdr* sh(int i) { return &g_sh[i]; }
static const char* shname(Elf32_Shdr* s) {
	Elf32_Shdr* str = sh(g_eh->e_shstrndx);
	return (const char*) (g_elf + str->sh_offset + s->sh_name);
}

/* Growable byte buffer. */
typedef struct { unsigned char* p; unsigned len, cap; } Buf;
static void bput(Buf* b, const void* d, unsigned n) {
	if (b->len + n > b->cap) { b->cap = (b->len + n) * 2 + 64; b->p = realloc(b->p, b->cap); }
	memcpy(b->p + b->len, d, n); b->len += n;
}
static unsigned bu32(Buf* b, unsigned v) { unsigned o = b->len; bput(b, &v, 4); return o; }

int main(int argc, char** argv) {
	const char* in = 0; const char* out = 0;
	int isDll = 0, exportAll = 0;
	char* wantExport[512]; int nWantExport = 0;
	char* needs[64]; int nNeeds = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dll")) isDll = 1;
		else if (!strcmp(argv[i], "--export-all")) exportAll = 1;
		else if (!strcmp(argv[i], "--export") && i + 1 < argc) wantExport[nWantExport++] = argv[++i];
		else if (!strcmp(argv[i], "--need") && i + 1 < argc) needs[nNeeds++] = argv[++i];
		else if (!in) in = argv[i];
		else if (!out) out = argv[i];
	}
	if (!in || !out) die("usage: mknx <in.elf> <out> [--dll] [--export N|--export-all] [--need N]");

	/* Read the whole ELF. */
	FILE* f = fopen(in, "rb");
	if (!f) die("cannot open input");
	fseek(f, 0, SEEK_END); g_elfLen = ftell(f); fseek(f, 0, SEEK_SET);
	g_elf = malloc(g_elfLen);
	if (fread(g_elf, 1, g_elfLen, f) != (size_t) g_elfLen) die("short read");
	fclose(f);

	g_eh = (Elf32_Ehdr*) g_elf;
	if (memcmp(g_eh->e_ident, "\177ELF", 4) != 0) die("not an ELF");
	if (g_eh->e_machine != EM_386) die("not i386");
	g_sh = (Elf32_Shdr*) (g_elf + g_eh->e_shoff);
	g_nsh = g_eh->e_shnum;

	/* 1) Image extent: loadBase = min SHF_ALLOC vaddr; bssStart = end of allocated
	 *    PROGBITS; bssEnd = end of all allocated sections (incl NOBITS .bss). */
	uint32_t loadBase = 0xFFFFFFFFu, bssStart = 0, bssEnd = 0;
	for (int i = 0; i < g_nsh; i++) {
		Elf32_Shdr* s = sh(i);
		if (!(s->sh_flags & SHF_ALLOC) || s->sh_size == 0) continue;
		if (s->sh_addr < loadBase) loadBase = s->sh_addr;
		uint32_t end = s->sh_addr + s->sh_size;
		if (end > bssEnd) bssEnd = end;
		if (s->sh_type != SHT_NOBITS && end > bssStart) bssStart = end;
	}
	if (loadBase == 0xFFFFFFFFu) die("no allocatable sections");
	uint32_t imageSize = bssStart - loadBase;

	/* Flat load image [loadBase, bssStart): copy each allocated PROGBITS section. */
	unsigned char* image = calloc(1, imageSize);
	for (int i = 0; i < g_nsh; i++) {
		Elf32_Shdr* s = sh(i);
		if ((s->sh_flags & SHF_ALLOC) && s->sh_type == SHT_PROGBITS && s->sh_size)
			memcpy(image + (s->sh_addr - loadBase), g_elf + s->sh_offset, s->sh_size);
	}

	/* Locate symtab + its strtab (for exports). */
	Elf32_Sym* sym = 0; int nsym = 0; const char* symstr = 0;
	for (int i = 0; i < g_nsh; i++)
		if (sh(i)->sh_type == SHT_SYMTAB) {
			sym = (Elf32_Sym*) (g_elf + sh(i)->sh_offset);
			nsym = sh(i)->sh_size / sizeof(Elf32_Sym);
			symstr = (const char*) (g_elf + sh(sh(i)->sh_link)->sh_offset);
		}

	/* 2) Relocations: R_386_32 sites in allocated target sections -> NxReloc[]. */
	Buf relocs = {0};
	unsigned relocCount = 0;
	for (int i = 0; i < g_nsh; i++) {
		Elf32_Shdr* s = sh(i);
		if (s->sh_type != SHT_REL) continue;
		Elf32_Shdr* tgt = sh(s->sh_info);
		if (!(tgt->sh_flags & SHF_ALLOC)) continue;
		Elf32_Rel* r = (Elf32_Rel*) (g_elf + s->sh_offset);
		int n = s->sh_size / sizeof(Elf32_Rel);
		for (int j = 0; j < n; j++) {
			if (ELF32_R_TYPE(r[j].r_info) != R_386_32) continue;
			if (r[j].r_offset < loadBase || r[j].r_offset >= bssStart) continue;
			bu32(&relocs, r[j].r_offset);
			relocCount++;
		}
	}

	/* 3) Exports (.ndl): named global symbols. String pool collects names. */
	Buf strs = {0};
	bput(&strs, "", 1);   /* offset 0 = empty string */
	Buf exports = {0};
	unsigned exportCount = 0;
	if (isDll && sym) {
		for (int i = 0; i < nsym; i++) {
			if (ELF32_ST_BIND(sym[i].st_info) != STB_GLOBAL) continue;
			if (sym[i].st_shndx == 0) continue;   /* undefined */
			int t = ELF32_ST_TYPE(sym[i].st_info);
			if (t != 1 /*OBJECT*/ && t != 2 /*FUNC*/) continue;
			const char* nm = symstr + sym[i].st_name;
			if (!nm[0]) continue;
			int want = exportAll;
			for (int k = 0; !want && k < nWantExport; k++)
				if (!strcmp(nm, wantExport[k])) want = 1;
			if (!want) continue;
			unsigned nameOff = strs.len;
			bput(&strs, nm, strlen(nm) + 1);
			NxExport e = { nameOff, sym[i].st_value };   /* nameOff fixed up below */
			bput(&exports, &e, sizeof e);
			exportCount++;
		}
	}

	/* 4) Imports (from a .nximports section: NxImport[] referencing in-image name strings
	 *    via abs addresses) and 5) needed libraries (--need). For Stage 1 EXEs these are
	 *    empty; the .nximports section is copied verbatim if present. */
	Buf imports = {0};
	unsigned importCount = 0;
	for (int i = 0; i < g_nsh; i++)
		if (!strcmp(shname(sh(i)), ".nximports") && sh(i)->sh_size) {
			bput(&imports, g_elf + sh(i)->sh_offset, sh(i)->sh_size);
			importCount = sh(i)->sh_size / sizeof(NxImport);
		}
	Buf needed = {0};
	for (int i = 0; i < nNeeds; i++) {
		unsigned nameOff = strs.len;
		bput(&strs, needs[i], strlen(needs[i]) + 1);
		NxNeeded nd = { nameOff };
		bput(&needed, &nd, sizeof nd);
	}

	/* Lay the tables out in the file AFTER the load image (at vaddr bssStart onward); the
	 * loader reads them from the staged file before zeroing bss, so they need not be
	 * mapped at runtime. Table fields in the header are absolute vaddrs (loadBase + file
	 * offset). String offsets recorded above are relative to the string pool start; fix
	 * them to absolute vaddrs now. */
	uint32_t cur = bssStart;                 /* vaddr cursor for appended tables */
	uint32_t impAddr = cur; cur += imports.len;
	uint32_t expAddr = cur; cur += exports.len;
	uint32_t relAddr = cur; cur += relocs.len;
	uint32_t needAddr = cur; cur += needed.len;
	uint32_t strAddr = cur; cur += strs.len;

	/* Fix export/needed nameOff (string-pool-relative -> absolute vaddr). */
	NxExport* ex = (NxExport*) exports.p;
	for (unsigned i = 0; i < exportCount; i++) ex[i].nameOff += strAddr;
	NxNeeded* nn = (NxNeeded*) needed.p;
	for (int i = 0; i < nNeeds; i++) nn[i].nameOff += strAddr;

	/* Header lives in the first bytes of the load image (the .nxheader placeholder). */
	NxHeader h;
	memset(&h, 0, sizeof h);
	h.magic = NX_MAGIC; h.version = NX_VERSION; h.flags = isDll ? NX_FLAG_DLL : 0;
	h.entry = g_eh->e_entry; h.loadBase = loadBase; h.imageSize = imageSize;
	h.bssStart = bssStart; h.bssEnd = bssEnd;
	h.importTable = importCount ? impAddr : 0; h.importCount = importCount;
	h.exportTable = exportCount ? expAddr : 0; h.exportCount = exportCount;
	h.relocTable = relocCount ? relAddr : 0;   h.relocCount = relocCount;
	h.neededTable = nNeeds ? needAddr : 0;     h.neededCount = nNeeds;
	if (imageSize < sizeof(NxHeader)) die("image smaller than header");
	memcpy(image, &h, sizeof h);

	/* Write: [image][imports][exports][relocs][needed][strings]. */
	FILE* o = fopen(out, "wb");
	if (!o) die("cannot open output");
	fwrite(image, 1, imageSize, o);
	fwrite(imports.p, 1, imports.len, o);
	fwrite(exports.p, 1, exports.len, o);
	fwrite(relocs.p, 1, relocs.len, o);
	fwrite(needed.p, 1, needed.len, o);
	fwrite(strs.p, 1, strs.len, o);
	fclose(o);
	return 0;
}
