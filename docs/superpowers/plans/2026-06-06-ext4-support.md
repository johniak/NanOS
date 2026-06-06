# ext4 Read-Only Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add ext4 read-only support behind a shared `ExtFilesystem` core, with VFS auto-detection, and boot/mount the system from an ext4 image while keeping ext2.

**Architecture:** Factor the existing ext2 reader into an `ExtFilesystem` base (superblock/BGD/inode/dir/read machinery) exposing one virtual seam `resolveBlock(inode, fileBlockIndex)`. `Ext2Filesystem` maps via direct blocks; `Ext4Filesystem` walks an extent tree and handles 64-bit descriptors. `FileSystemType` gains `probe()` so `Vfs::mount(..., "auto", ...)` picks the right driver from the superblock.

**Tech Stack:** C++ freestanding (i686), doctest host tests, lcov ≥90% gate, RamBlockDevice fixtures, Docker build, QEMU.

**Spec:** `docs/superpowers/specs/2026-06-06-ext4-support-design.md`

**Conventions reused:**
- Host tests run via `make test` (native image, copies to case-sensitive `/tmp/nbuild`, gate ≥90%).
- Kernel build via `make build` (case-sensitive copy `/tmp/nanos-ksrc`). Boot check via `make image` + headless QEMU screendump.
- Test devices/instances must have static/heap lifetime (the VFS/registry keep pointers).
- `TEST_MODULES`/`COV_PATTERNS` in `Makefile` list modules under test; new modules get added there.

---

### Task 1: Extract `ExtFilesystem` core; make `Ext2Filesystem` a subclass

**Files:**
- Create: `fs/ExtFilesystem.h`, `fs/ExtFilesystem.cpp`
- Modify: `fs/Ext2Filesystem.h` (becomes thin subclass), `Makefile` (SOURCES `Ext2Filesystem.o`→`ExtFilesystem.o`; TEST_MODULES `fs/Ext2Filesystem.cpp`→`fs/ExtFilesystem.cpp`; COV_PATTERNS add ExtFilesystem)
- Test: existing `tests/test_ext2.cpp` must stay green (it now exercises the core).

- [ ] **Step 1: Create `fs/ExtFilesystem.h`** — move ALL current logic from `Ext2Filesystem.h` into a base class `ExtFilesystem : public FileSystem`, with the struct definitions (Ext2*Superblock*, Ext2Inode, Ext2DirectoryEntry, BGD) kept here. Add the virtual seam and route block reads through it:

```cpp
#pragma once
#include "Vfs.h"
#include "Console.h"
#include "string.h"
#include "List.h"
#include "String.h"

namespace kernel {
int ceil(float num);
// ... (move Ext2BaseSuperblockFields, Ext2ExtendedSuperblockFields,
//      Ext2BlockGroupDescriptor, Ext2Inode, Ext2DirectoryEntry here unchanged) ...

class ExtFilesystem : public FileSystem {
protected:
	BlockDevice* device;
	char superblockBuff[1024];
	char commonBuff[4096];
	Ext2BaseSuperblockFields baseSuperBlock;
	Ext2ExtendedSuperblockFields extendedSuperblock;
	Ext2BlockGroupDescriptor blockGroupDescriptors[100];
	int blockGroupsCount;
	int partitionLba;
	int blockSize;
	int inodeSize;
	int descSize;
public:
	ExtFilesystem(BlockDevice* device, unsigned partitionLba) {
		this->device = device; this->partitionLba = partitionLba;
	}
	// file-block index -> absolute fs block number. Specialised per fs.
	virtual unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) = 0;

	int mount() {
		device->readSectors(this->partitionLba + 2, 2, superblockBuff);
		memcpy(&baseSuperBlock, superblockBuff, sizeof(Ext2BaseSuperblockFields));
		memcpy(&extendedSuperblock, superblockBuff + sizeof(Ext2BaseSuperblockFields),
				sizeof(Ext2ExtendedSuperblockFields));
		inodeSize = extendedSuperblock.sizeOfInodeStructure;
		if (inodeSize <= 0) inodeSize = 128;
		// 64-bit feature -> 64-byte descriptors; s_desc_size at superblock byte 0xFE.
		descSize = 32;
		unsigned incompat = *(unsigned*)(superblockBuff + 0x60);
		if (incompat & 0x80) {
			unsigned short ds = *(unsigned short*)(superblockBuff + 0xFE);
			if (ds >= 32) descSize = ds;
		}
		initBgdt();
		printInfo();
		return 0;
	}
	void initBgdt() {
		blockGroupsCount = (int) ceil(((float) baseSuperBlock.totalBlocks)
				/ ((float) baseSuperBlock.blockInGroup));
		blockSize = 1024 << baseSuperBlock.log2BlockSize;
		// BGD table starts in the block after the superblock.
		int bgdtBlock = (blockSize == 1024) ? 2 : 1;
		device->readSectors(this->partitionLba + bgdtBlock * (blockSize / 512),
				(blockGroupsCount * descSize + 511) / 512 + 1, commonBuff);
		for (int g = 0; g < blockGroupsCount; g++)
			memcpy(&blockGroupDescriptors[g], commonBuff + g * descSize,
					sizeof(Ext2BlockGroupDescriptor));
	}
	// ... move printInfo, getInode, getFileType, getDirectoriesEntries,
	//     getInodeByPath, getChildrenInode, isDirectory, ls,
	//     read/stat/readdir, readFile(Ext2Inode,...) here UNCHANGED, except:
	//     in readFile and getDirectoriesEntries replace `inode.directBlocks[i]`
	//     with `resolveBlock(inode, i)`.
};
}
```

NOTE: in `getDirectoriesEntries` the block is `resolveBlock(inode, 0)`; in `readFile`'s loop, `unsigned blockAddress = resolveBlock(inode, i);`.

- [ ] **Step 2: Create `fs/ExtFilesystem.cpp`** with `ceil` (moved from Ext2Filesystem.cpp):

```cpp
#include "ExtFilesystem.h"
namespace kernel {
int ceil(float num) {
	int inum = (int) num;
	if (num == (float) inum) return inum;
	return inum + 1;
}
}
```

- [ ] **Step 3: Rewrite `fs/Ext2Filesystem.h`** as a thin subclass:

```cpp
#pragma once
#include "ExtFilesystem.h"
namespace kernel {
class Ext2Filesystem : public ExtFilesystem {
public:
	Ext2Filesystem(BlockDevice* device, unsigned partitionLba)
		: ExtFilesystem(device, partitionLba) {}
	unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) {
		// direct blocks only (read-only, blocks 0-11) — existing behaviour
		return inode.directBlocks[fileBlockIndex];
	}
};
class Ext2FileSystemType : public FileSystemType {
public:
	const char* name() { return "ext2"; }
	FileSystem* create(BlockDevice* dev, unsigned partitionLba) {
		return new Ext2Filesystem(dev, partitionLba);
	}
	// added in Task 2
};
}
```

- [ ] **Step 4: Delete `fs/Ext2Filesystem.cpp`** (ceil moved). `git rm fs/Ext2Filesystem.cpp`.

- [ ] **Step 5: Update Makefile** — SOURCES: replace `Ext2Filesystem.o` with `ExtFilesystem.o`. TEST_MODULES: replace `fs/Ext2Filesystem.cpp` with `fs/ExtFilesystem.cpp`. COV_PATTERNS: add `"*/ExtFilesystem.*"`.

- [ ] **Step 6: Run host tests — expect GREEN (ext2 unchanged behaviour).**

Run: `make test`
Expected: 25/25 pass, coverage ≥90% (ExtFilesystem core now covered by ext2 tests).

- [ ] **Step 7: Build kernel — expect it compiles.**

Run: `make build`
Expected: `bin/kernel.bin` produced, no errors.

- [ ] **Step 8: Commit**

```bash
git add -A
git commit -m "refactor: extract ExtFilesystem core; Ext2Filesystem maps direct blocks"
```

---

### Task 2: `probe()` on `FileSystemType` + VFS `"auto"` mount

**Files:**
- Modify: `fs/Vfs.h` (add `probe` to `FileSystemType`), `fs/Vfs.cpp` (`mount` handles `"auto"`), `fs/Ext2Filesystem.h` (`Ext2FileSystemType::probe`), `tests/test_vfs.cpp` (FakeType gains `probe`; new auto tests)

- [ ] **Step 1: Write failing tests** in `tests/test_vfs.cpp` — extend `FakeType` with a `probe` returning a settable flag, and add:

```cpp
// in FakeType:
bool probeResult = true;
bool probe(BlockDevice*, unsigned) { return probeResult; }

TEST_CASE("Vfs auto-mount picks the first type whose probe matches") {
	Vfs vfs;
	FakeType a("afs"); a.probeResult = false;
	FakeType b("bfs"); b.probeResult = true;
	vfs.registerType(&a);
	vfs.registerType(&b);
	CHECK(vfs.mount("/", "auto", (BlockDevice*)0, 0) == 0);
	CHECK(b.made != 0);     // b matched and was created
	CHECK(a.made == 0);
}

TEST_CASE("Vfs auto-mount fails when no type probes true") {
	Vfs vfs;
	FakeType a("afs"); a.probeResult = false;
	vfs.registerType(&a);
	CHECK(vfs.mount("/", "auto", (BlockDevice*)0, 0) < 0);
}
```

- [ ] **Step 2: Run — expect FAIL** (probe pure-virtual not implemented / "auto" unhandled).

Run: `make test`
Expected: compile error (FakeType lacks probe override) then, once added, the auto cases fail until mount handles "auto".

- [ ] **Step 3: Add `probe` to `FileSystemType`** in `fs/Vfs.h`:

```cpp
class FileSystemType {
public:
	virtual ~FileSystemType() {}
	virtual const char* name() = 0;
	virtual bool probe(BlockDevice* dev, unsigned partitionLba) = 0;
	virtual FileSystem* create(BlockDevice* dev, unsigned partitionLba) = 0;
};
```

- [ ] **Step 4: Handle `"auto"` in `Vfs::mount`** (`fs/Vfs.cpp`) — before the name match, if fstype is "auto", pick the first type whose `probe` is true:

```cpp
int Vfs::mount(String mountpoint, String fstype, BlockDevice* dev, unsigned partitionLba) {
	const char* wanted = (char*) fstype;
	FileSystemType* type = 0;
	bool autodetect = (strcmp(wanted, "auto") == 0);
	for (int i = 0; i < types.getCount(); i++) {
		if (autodetect) {
			if (types[i]->probe(dev, partitionLba)) { type = types[i]; break; }
		} else if (strcmp(types[i]->name(), wanted) == 0) {
			type = types[i]; break;
		}
	}
	if (type == 0) return -1;
	/* ... unchanged: create, mount, add to table ... */
}
```

- [ ] **Step 5: Implement `Ext2FileSystemType::probe`** in `fs/Ext2Filesystem.h` — superblock magic 0xEF53 (byte 0x38 of the superblock = partition byte 1024+56) AND no extents/64bit incompat flags:

```cpp
bool probe(BlockDevice* dev, unsigned partitionLba) {
	char sb[1024];
	dev->readSectors(partitionLba + 2, 2, sb);
	unsigned short magic = *(unsigned short*)(sb + 0x38);
	unsigned incompat = *(unsigned*)(sb + 0x60);
	return magic == 0xEF53 && !(incompat & (0x40 | 0x80));   // not EXTENTS|64BIT
}
```

- [ ] **Step 6: Run — expect GREEN.**

Run: `make test`
Expected: all pass incl. the two new auto-mount cases.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: VFS probe-based auto-detection of filesystem type"
```

---

### Task 3: `Ext4Filesystem` (extents + 64-bit) + fixture + tests

**Files:**
- Create: `fs/Ext4Filesystem.h`, `tests/test_ext4.cpp`, `tests/fixtures/ext4.img`
- Modify: `Makefile` (TEST_MODULES already compiles fs via ExtFilesystem.cpp; COV_PATTERNS add `"*/Ext4Filesystem.*"`)

- [ ] **Step 1: Create the ext4 fixture** (raw mkfs.ext4 with known files incl. a >1-block file to exercise extents):

```bash
printf 'Hello, NanOS VFS!\n' > /tmp/hello.txt
printf 'set timeout=5\nset default=0\n\nmenuentry "NanOS" {\n    multiboot /boot/kernel.bin\n}\n' > /tmp/grub.cfg
# a file larger than one 1K block (e.g. 5000 bytes of repeating content)
python3 - <<'PY'
open('/tmp/big.txt','w').write(('NANOS-EXT4-EXTENT-TEST-' * 250)[:5000])
PY
docker run --rm --platform linux/amd64 -v "$PWD":/src \
  -v /tmp/hello.txt:/in/hello.txt:ro -v /tmp/grub.cfg:/in/grub.cfg:ro -v /tmp/big.txt:/in/big.txt:ro \
  -w /src nanos-build bash -c '
  dd if=/dev/zero of=tests/fixtures/ext4.img bs=1024 count=4096 status=none
  mkfs.ext4 -F -q -b 1024 tests/fixtures/ext4.img
  debugfs -w -R "mkdir /boot" tests/fixtures/ext4.img
  debugfs -w -R "mkdir /boot/grub" tests/fixtures/ext4.img
  debugfs -w -R "write /in/hello.txt hello.txt" tests/fixtures/ext4.img
  debugfs -w -R "write /in/grub.cfg /boot/grub/grub.cfg" tests/fixtures/ext4.img
  debugfs -w -R "write /in/big.txt big.txt" tests/fixtures/ext4.img
  dumpe2fs -h tests/fixtures/ext4.img 2>/dev/null | grep -iE "Inode size|Filesystem features"
'
```
Expected: "Filesystem features:" lists `extent 64bit ...`.

- [ ] **Step 2: Write failing tests** `tests/test_ext4.cpp`:

```cpp
#include "doctest.h"
#include "Ext4Filesystem.h"
#include "RamBlockDevice.h"
#include "Vfs.h"
#include <cstdio>
#include <cstring>
using namespace kernel;

static RamBlockDevice* loadExt4() {
	FILE* f = fopen("tests/fixtures/ext4.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	return new RamBlockDevice("ext4", buf, (unsigned) sz);
}

TEST_CASE("ext4 mounts and reads a small file via extents") {
	Ext4Filesystem fs(loadExt4(), 0);
	REQUIRE(fs.mount() == 0);
	char buf[64] = {0};
	int n = fs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0); buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}

TEST_CASE("ext4 readdir lists entries") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	List<DirEntry> e;
	REQUIRE(fs.readdir("/boot/grub", e) == 0);
	bool found = false;
	for (int i = 0; i < e.getCount(); i++)
		if (strcmp(e[i].name, "grub.cfg") == 0) found = true;
	CHECK(found);
}

TEST_CASE("ext4 reads a multi-block file fully (extent spanning)") {
	Ext4Filesystem fs(loadExt4(), 0);
	fs.mount();
	FileStat st;
	REQUIRE(fs.stat("/big.txt", st) == 0);
	CHECK(st.size == 5000);
	char* buf = (char*) malloc(st.size + 1);
	int n = fs.read("/big.txt", st.size, 0, buf);
	CHECK(n == (int) st.size);
	CHECK(buf[0] == 'N');
	CHECK(buf[4999] != 0);   // last byte present (not a hole)
}

TEST_CASE("ext4 type probes true on ext4, ext2 probes false") {
	Ext4FileSystemType e4; Ext2FileSystemType e2;
	RamBlockDevice* dev = loadExt4();
	CHECK(e4.probe(dev, 0) == true);
	CHECK(e2.probe(dev, 0) == false);
}

TEST_CASE("VFS auto-mount resolves ext4 image to the ext4 driver") {
	Vfs vfs;
	static Ext2FileSystemType e2; static Ext4FileSystemType e4;
	vfs.registerType(&e4); vfs.registerType(&e2);
	REQUIRE(vfs.mount("/", "auto", loadExt4(), 0) == 0);
	char buf[64] = {0};
	int n = vfs.read("/hello.txt", sizeof(buf) - 1, 0, buf);
	REQUIRE(n > 0); buf[n] = 0;
	CHECK(strcmp(buf, "Hello, NanOS VFS!\n") == 0);
}
```

Also add the cross-check to `tests/test_ext2.cpp`: ext4 type probes FALSE on the ext2 fixture (so auto-detect is unambiguous):
```cpp
TEST_CASE("ext4 type does not claim the ext2 image") {
	// (loadFixture() defined in this file loads the ext2 fixture)
	Ext4FileSystemType e4;
	CHECK(e4.probe(loadFixture(), 0) == false);
}
```
(Add `#include "Ext4Filesystem.h"` to test_ext2.cpp.)

- [ ] **Step 3: Run — expect FAIL** (`Ext4Filesystem.h` missing).

Run: `make test`
Expected: `Ext4Filesystem.h: No such file or directory`.

- [ ] **Step 4: Implement `fs/Ext4Filesystem.h`** — extent structs + tree walk + type with probe:

```cpp
#pragma once
#include "ExtFilesystem.h"

namespace kernel {

struct Ext4ExtentHeader { unsigned short magic; unsigned short entries;
	unsigned short max; unsigned short depth; unsigned generation; };
struct Ext4ExtentIdx { unsigned fileBlock; unsigned leafLo;
	unsigned short leafHi; unsigned short unused; };
struct Ext4Extent { unsigned fileBlock; unsigned short len;
	unsigned short startHi; unsigned startLo; };

class Ext4Filesystem : public ExtFilesystem {
	char extentBuf[4096];
public:
	Ext4Filesystem(BlockDevice* device, unsigned partitionLba)
		: ExtFilesystem(device, partitionLba) {}

	unsigned resolveBlock(Ext2Inode& inode, unsigned fileBlockIndex) {
		// Inodes without the extents flag use classic direct blocks.
		if (!(inode.flags & 0x80000))
			return inode.directBlocks[fileBlockIndex];
		// The 60-byte i_block area starts at directBlocks[0].
		char node[60];
		memcpy(node, &inode.directBlocks[0], 60);
		char* cur = node;
		while (true) {
			Ext4ExtentHeader* h = (Ext4ExtentHeader*) cur;
			if (h->magic != 0xF30A) return 0;
			if (h->depth == 0) {
				Ext4Extent* ex = (Ext4Extent*) (cur + sizeof(Ext4ExtentHeader));
				for (int i = 0; i < h->entries; i++) {
					unsigned start = ex[i].fileBlock;
					unsigned end = start + ex[i].len;       // len fits (small files)
					if (fileBlockIndex >= start && fileBlockIndex < end)
						return ex[i].startLo + (fileBlockIndex - start);
				}
				return 0;
			}
			// internal node: descend into the index covering fileBlockIndex
			Ext4ExtentIdx* ix = (Ext4ExtentIdx*) (cur + sizeof(Ext4ExtentHeader));
			int pick = 0;
			for (int i = 0; i < h->entries; i++)
				if (ix[i].fileBlock <= fileBlockIndex) pick = i;
			unsigned child = ix[pick].leafLo;
			device->readSectors(this->partitionLba + child * (blockSize / 512),
					(blockSize / 512), extentBuf);
			cur = extentBuf;
		}
	}
};

class Ext4FileSystemType : public FileSystemType {
public:
	const char* name() { return "ext4"; }
	bool probe(BlockDevice* dev, unsigned partitionLba) {
		char sb[1024];
		dev->readSectors(partitionLba + 2, 2, sb);
		unsigned short magic = *(unsigned short*)(sb + 0x38);
		unsigned incompat = *(unsigned*)(sb + 0x60);
		return magic == 0xEF53 && (incompat & (0x40 | 0x80));   // EXTENTS|64BIT
	}
	FileSystem* create(BlockDevice* dev, unsigned partitionLba) {
		return new Ext4Filesystem(dev, partitionLba);
	}
};
}
```

(`device`, `partitionLba`, `blockSize` are `protected` in `ExtFilesystem` — accessible.)

- [ ] **Step 5: Update Makefile** COV_PATTERNS add `"*/Ext4Filesystem.*"`.

- [ ] **Step 6: Run — expect GREEN, coverage ≥90%.**

Run: `make test`
Expected: ext4 cases pass (incl. multi-block extent read), ext2 cases still pass, gate ≥90%.

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: ext4 read-only filesystem (extents + 64-bit) with fixture and tests"
```

---

### Task 4: Boot from ext4 — image + kernel wiring + QEMU verify

**Files:**
- Modify: `scripts/create-grub2-image.sh` (`mke2fs -t ext2` → `mkfs.ext4`), `kernel/Kernel.cpp` (register `Ext4FileSystemType`, mount `"auto"`), `Makefile` (no new SOURCES — ext4 header-only)

- [ ] **Step 1: Switch image to ext4** in `scripts/create-grub2-image.sh` — replace the mke2fs line:

```sh
# was: mke2fs -t ext2 -q -E offset=$OFFSET $IMG ${PART_BLOCKS}k
mke2fs -t ext4 -q -E offset=$OFFSET $IMG ${PART_BLOCKS}k
```

- [ ] **Step 2: Wire ext4 into the kernel** (`kernel/Kernel.cpp`) — include and register the ext4 type, mount "auto":

```cpp
#include "Ext4Filesystem.h"   // near the other fs includes
// ... in start(), replace the mount block:
	Vfs* vfs = new Vfs();
	vfs->registerType(new Ext4FileSystemType());
	vfs->registerType(new Ext2FileSystemType());
	vfs->mount("/", "auto", hd0, 2048);
```

- [ ] **Step 3: Build the ext4 image and boot headless** (timeout=0 for a clean capture):

```bash
cp grub.cfg /tmp/gcfg.bak
printf 'set timeout=0\nset default=0\n\nmenuentry "NanOS" {\n    multiboot /boot/kernel.bin\n}\n' > grub.cfg
rm -f disk/image-grub2.img
make image
qemu-system-i386 -drive file=disk/image-grub2.img,format=raw -display none \
  -monitor unix:/tmp/qmon,server,nowait &
# screendump after ~10s via the monitor socket; convert ppm->png; inspect
cp /tmp/gcfg.bak grub.cfg
```
Expected on screen: ext2/ext4 init banner, `Contents of /boot/grub:` listing `grub.cfg`, and the printed `grub.cfg` contents — proving GRUB2 booted from ext4 and the kernel auto-mounted ext4.

- [ ] **Step 4: If GRUB cannot read the ext4 partition** (boot stalls before the kernel banner): re-run with the partition kept ext2 but confirm the kernel still auto-detects ext2; document the GRUB limitation in the spec Risks and keep ext4 for data partitions. (Only if Step 3 fails.)

- [ ] **Step 5: Run host tests once more** to confirm nothing regressed.

Run: `make test`
Expected: all pass, ≥90%.

- [ ] **Step 6: Commit**

```bash
git add -A
git commit -m "feat: boot from an ext4 image and auto-mount it in the kernel"
```

---

## Self-Review notes
- **Spec coverage:** shared core (Task 1), probe/auto (Task 2), extents+64bit+fixture+tests (Task 3), ext4-everywhere image+wiring+QEMU (Task 4). All spec sections mapped.
- **Type consistency:** `resolveBlock(Ext2Inode&, unsigned)` defined in Task 1, overridden identically in Tasks 1 (ext2) and 3 (ext4). `probe(BlockDevice*, unsigned)` added in Task 2, implemented in Tasks 2 (ext2) and 3 (ext4). `FakeType` gains `probe` in Task 2.
- **Known sharp edges:** extent `len` high bit (uninitialized/large extents) — fixture files are small so `len` is plain; documented. GRUB-reads-ext4 is the main risk, with a fallback step.
