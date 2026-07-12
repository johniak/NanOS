# Fix ext-path UB + kernel -O2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Usunąć latentny OOB/UB w rozwiązywaniu ścieżek (warstwa ext + `String`), przez który
cały kernel od tygodni buduje się na `-O0`, i trwale włączyć `KOPTFLAGS=-O2`.

**Architecture:** Najpierw infrastruktura diagnostyczna (sanitized host-test target), potem
TDD-owe usunięcie trzech *zidentyfikowanych* defektów (mutujący `String::operator+`,
`operator char*()` zwracający nullptr, nieograniczone skany w `ExtFilesystem`), na końcu
reprodukcja -O2 w QEMU z procedurą bisekcji per-TU, gdyby triple-fault przetrwał, i flip flagi.

**Tech Stack:** C++17 (freestanding kernel, gcc cross w Dockerze), doctest na hoście
(`make test64` → `_test` w kontenerze), ASan/UBSan (host, Linux-kontener), QEMU x86_64.

## Global Constraints

- Commity **bez wzmianki o AI** (żadnych Co-Authored-By/„Generated with").
- Gałąź robocza: `fix/ext-path-ub-o2` od `main` (nie od `feat/i915-dell-gpu`).
- Bramki przed PR: `make test64` (bramka ≥90% lcov, `COV_MIN=90`, Makefile:2716)
  oraz `make verify64` zielone.
- Build jądra idzie przez Dockera z kopii na case-sensitive FS (`/tmp/nanos-ksrc`) —
  edytuj źródła w repo, nie w kopii.
- Host testy budują się BEZ `-Iinclude` (żeby `<string.h>` był libc, nie freestanding) —
  nowe testy includują nagłówki tak jak `tests/test_ext2.cpp`.
- Nie dotykamy `linuxkpi/` ani `external/` (żywa kampania i915).

## Stan wiedzy (dlaczego te taski)

- `Makefile:1263-1267`: `KOPTFLAGS=` puste; komentarz obwinia „basename/dirname loop reads
  past a non-NUL-terminated String buffer" w ext path-resolution — triple-fault przy
  boot-time root mount pod `-O2`.
- Audyt 2026-07-04 ustalił, że diagnoza z komentarza jest co najmniej niepełna: **każda**
  ścieżka konstrukcji/append w `lib/String.h` NUL-terminuje bufor (`String.h:23,41,54,68`).
  Realne, potwierdzone defekty w okolicy:
  1. **`String::operator+` mutuje lewy operand** (`String.h:131-142` — woła `this->append`
     i zwraca kopię). `c = a + b` niszczy `a`. Przy -O2 inaczej składane temporaries /
     eliminacje kopii zmieniają, KTÓRY obiekt zostaje zmutowany → use-after-free/underrun
     w zupełnie innym miejscu niż przyczyna.
  2. **`String()` (domyślny) ma `textArray == nullptr`** (`String.cpp:11-14`), a
     `operator char*()` (`String.h:128-130`) zwraca go wprost; `compareTo`/`indexOf`
     dereferencują bez guardu. `ExtFilesystem::resolvePath` łapie `!p`, ale inne call-site'y
     `(char*) path` — nie.
  3. **Nieograniczone skany** w `fs/ExtFilesystem.h`: `resolvePath` (`:366-393`,
     `while (p[i])`/`while (p[j] && p[j] != '/')`) i `resolveParent` (`:1031-1044`,
     `while (path[end]) end++` — to jest dosłownie „basename/dirname loop" z komentarza).
     Przy zepsutym buforze wejściowym uciekają w niezmapowaną pamięć.
- Winowajcą -O2 może też być inny TU — stąd Task 5 ma pełną procedurę bisekcji, a Task 1
  daje sanitizery, które złapią OOB niezależnie od miejsca.

---

### Task 1: Target `test-san` — host-testy pod ASan/UBSan na -O2

**Files:**
- Modify: `Makefile` (część hostowa przy `test:`/`test64:`, Makefile:949-962; część
  kontenerowa po `_test`, Makefile:2722-2735)

**Interfaces:**
- Produces: `make test-san` — buduje CAŁĄ suitę doctest (te same `TEST_SRCS`/`TEST_MODULES`)
  z `-O2 -fsanitize=address,undefined` i uruchamia ją; exit code ≠ 0 przy każdym
  OOB/UB/failu testu. Bez bramki lcov (coverage i ASan się gryzą).

- [ ] **Step 1: Dodaj target kontenerowy `_test_san`** — w części kontenerowej Makefile,
  bezpośrednio pod `_coverage` (Makefile:2736), przed `endif`:

```make
# Sanitized run: the same doctest suite at -O2 under ASan+UBSan. This is the oracle for
# latent OOB/UB (the ext path-scan bug class) — optimization-sensitive bugs surface here
# instead of triple-faulting QEMU. No lcov gate (coverage + ASan interfere).
HOST_SAN_FLAGS=-std=c++17 -O2 -g $(HINCLUDES) -Wall -DNX_FORCE64=1 -DNANOS_HOST_TEST=1 \
  -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
_test_san:
	@rm -rf $(BUILDDIR).san && mkdir -p $(BUILDDIR).san && \
	 tar -cf - --exclude=.git --exclude=disk --exclude=bin --exclude=iso --exclude=coverage -C /src . | tar -xf - -C $(BUILDDIR).san
	cd $(BUILDDIR).san && $(HOST_CXX) $(HOST_SAN_FLAGS) -c $(TEST_SRCS) $(TEST_MODULES)
	cd $(BUILDDIR).san && $(HOST_CXX) $(HOST_SAN_FLAGS) -o $(TEST_BIN).san *.o
	cd $(BUILDDIR).san && ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 $(TEST_BIN).san
```

  (`detect_leaks=0`: kod jądra w testach leakuje z założenia — chcemy OOB/UB, nie leaki.)

- [ ] **Step 2: Dodaj wrapper hostowy** — obok `test64:` (Makefile:961):

```make
test-san: test-image
	$(TEST_DOCKER_RUN) make ARCH=x86_64 _test_san
```

- [ ] **Step 3: Uruchom i zapisz wynik.**
  Run: `make test-san 2>&1 | tail -40`
  Expected: jeden z dwóch wyników, oba wartościowe: (a) suite PASS → istniejące testy nie
  trafiają w UB, diagnoza pójdzie przez Task 2; (b) raport ASan/UBSan ze stacktrace →
  **to jest winowajca**; zanotuj go w tym pliku pod tym taskiem i dołącz do fixa w Task 3/4.
- [ ] **Step 4: Commit.**

```bash
git add Makefile
git commit -m "build: test-san target — host doctest suite at -O2 under ASan/UBSan"
```

### Task 2: Testy adversarialne ścieżek (host, najpierw czerwone)

**Files:**
- Create: `tests/test_path_hostile.cpp`
- Test: jw. (`TEST_SRCS` łapie wildcardem `tests/*.cpp` — Makefile:2676)

**Interfaces:**
- Consumes: fixture `tests/fixtures/ext2.img` + wzorzec `loadFixture()` z
  `tests/test_ext2.cpp:15-25`; API `Ext2Filesystem::{mount,stat}` (`fs/Ext2Filesystem.h`),
  `String` (`lib/String.h`).
- Produces: testy-strażnicy semantyki, na których Task 3 i 4 robią green.

- [ ] **Step 1: Napisz testy** — `tests/test_path_hostile.cpp`:

```cpp
#include "doctest.h"
#include "Ext2Filesystem.h"
#include "RamBlockDevice.h"
#include "String.h"
#include <cstdio>
#include <cstring>

using namespace kernel;

static RamBlockDevice* loadFixture() {
	FILE* f = fopen("tests/fixtures/ext2.img", "rb");
	REQUIRE(f != nullptr);
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char* buf = (char*) malloc(sz);
	REQUIRE(fread(buf, 1, sz, f) == (size_t) sz);
	fclose(f);
	return new RamBlockDevice("fixture", buf, (unsigned) sz);
}

// --- String semantics guards (drive the Task 3 fix) ---

TEST_CASE("String: operator+ must not mutate the left operand") {
	String a("/disks");
	String c = a + String("/main");
	CHECK(strcmp((char*) c, "/disks/main") == 0);
	CHECK(strcmp((char*) a, "/disks") == 0);   // FAILS today: a becomes "/disks/main"
	CHECK(a.getLenght() == 6);
}

TEST_CASE("String: default-constructed String yields a valid empty C-string") {
	String empty;
	char* p = (char*) empty;
	REQUIRE(p != nullptr);                      // FAILS today: textArray == 0
	CHECK(p[0] == 0);
	CHECK(empty.compareTo(String("")) == 0);    // crashes today (strcmp on nullptr)
}

// --- ext path-resolution guards (drive the Task 4 fix) ---

TEST_CASE("ext: hostile path shapes neither crash nor resolve") {
	Ext2Filesystem fs(loadFixture(), 0);
	REQUIRE(fs.mount() == 0);
	FileStat st;
	CHECK(fs.stat(String(), st) < 0);           // null-backed String
	CHECK(fs.stat(String(""), st) < 0);         // empty
	CHECK(fs.stat(String("no-slash"), st) < 0); // not absolute
	CHECK(fs.stat(String("/"), st) == 0);       // root resolves
	CHECK(fs.stat(String("//"), st) == 0);      // empty components skipped
	CHECK(fs.stat(String("/hello.txt/"), st) == 0);   // trailing slash tolerated
}

TEST_CASE("ext: oversized components and paths fail cleanly") {
	Ext2Filesystem fs(loadFixture(), 0);
	REQUIRE(fs.mount() == 0);
	char comp[300];
	memset(comp, 'a', sizeof(comp));
	comp[0] = '/';
	comp[299] = 0;                              // one 298-char component (>255 limit)
	FileStat st;
	CHECK(fs.stat(String(comp), st) < 0);
	char deep[5000];                            // >4096: must be rejected, not scanned
	unsigned i = 0;
	while (i + 2 < sizeof(deep) - 1) { deep[i++] = '/'; deep[i++] = 'x'; }
	deep[i] = 0;
	CHECK(fs.stat(String(deep), st) < 0);
}
```

- [ ] **Step 2: Zbuduj i potwierdź czerwone.**
  Run: `make test64 2>&1 | grep -E "FAILED|assert|ERRORS"`
  Expected: FAIL w `operator+ must not mutate` i `default-constructed String` (możliwy
  crash zamiast FAIL w `compareTo` — to też jest „czerwone"). Testy ext mogą już przechodzić
  (guard `!p` w `resolvePath`) — to OK, zostają jako strażnicy.
- [ ] **Step 3: Commit (tylko testy, znane czerwone — bez flag skip; naprawa w 2 następnych
  taskach na tej samej gałęzi przed PR).**

```bash
git add tests/test_path_hostile.cpp
git commit -m "test: hostile-path + String-semantics guards (red: operator+ mutation, null empty)"
```

### Task 3: Naprawa semantyki `String`

**Files:**
- Modify: `lib/String.h:107-145` (`indexOf`, `compareTo`, `operator char*`, `operator+`)
- Test: `tests/test_path_hostile.cpp` (Task 2), `tests/test_string.cpp` (istniejące)

**Interfaces:**
- Produces: `String::operator+(...) const` — niemutujący, zwraca nowy `String`;
  `(char*) String()` zwraca `""` (nigdy nullptr); `compareTo`/`indexOf`/`startsWith`
  bezpieczne dla pustego `String`. Sygnatury pozostają zgodne ze wszystkimi call-site'ami.

- [ ] **Step 1: Implementacja** — w `lib/String.h` dodaj prywatny helper i przepnij metody:

```cpp
	// Always-valid C view: a default-constructed String owns no buffer (textArray==0);
	// every reader must see "" instead of dereferencing null.
	const char* cstr() const {
		return textArray ? textArray : "";
	}
```

  Zamień ciała (zachowując dokładnie te sygnatury):

```cpp
	int indexOf(String str, int start) {
		if ((size_t) start >= length)
			return -1;
		char* ptr = strstr(textArray + start, str.cstr());
		if (ptr == 0)
			return -1;
		return (int) (ptr - textArray);
	}
	int compareTo(String str) {
		return strcmp(cstr(), str.cstr());
	}
	operator char*() {
		return (char*) cstr();
	}
	// Non-mutating concatenation: the old operator+ called this->append() and returned a
	// copy, so `c = a + b` silently destroyed `a` — under -O2 the surviving temporary
	// differs and the corruption moves. Concatenate into a fresh copy instead.
	String operator+(const String& str) const {
		String r(*this);
		r.append(str);
		return r;
	}
	String operator+(const char* str) const {
		String r(*this);
		r.append(String(str));
		return r;
	}
	String operator+(int dec) const {
		String r(*this);
		r.append(String(dec));
		return r;
	}
```

  Uwaga: NIE zmieniaj domyślnego konstruktora na malloc-ujący — globalne/statyczne `String`
  konstruowałyby się przed initem heapu jądra; `cstr()` załatwia invariant bez alokacji.
  Wolne funkcje `operator+` w `lib/String.cpp:19-26` zostają (działają na kopii by-value).

- [ ] **Step 2: Zielone testy String.**
  Run: `make test64 2>&1 | tail -15`
  Expected: `operator+ must not mutate` i `default-constructed String` PASS; całe
  `test_string.cpp` PASS; bramka coverage ≥90% PASS.
- [ ] **Step 3: Sanitized run.**
  Run: `make test-san 2>&1 | tail -15`
  Expected: PASS bez raportów ASan/UBSan (jeśli raport z Task 1 Step 3 wskazywał String —
  tu znika).
- [ ] **Step 4: Commit.**

```bash
git add lib/String.h
git commit -m "lib: String — non-mutating operator+, null-safe empty-string view"
```

### Task 4: Ograniczenie skanów ścieżek w ext

**Files:**
- Modify: `fs/ExtFilesystem.h:366-393` (`resolvePath`), `fs/ExtFilesystem.h:1031-1044`
  (`resolveParent`)
- Test: `tests/test_path_hostile.cpp` (przypadki `oversized`)

**Interfaces:**
- Produces: oba skany twardo ograniczone do `EXT_PATH_SCAN_MAX = 4096` bajtów; wejście
  dłuższe/niedoterminowane → `false` zamiast czytania poza bufor.

- [ ] **Step 1: `resolvePath`** — dodaj stałą w klasie (obok innych stałych) i bounded pętle:

```cpp
	// Hard ceiling for path scans: no caller has a legitimate path this long, and an
	// unterminated buffer must fail the lookup instead of walking off into unmapped memory.
	static const int EXT_PATH_SCAN_MAX = 4096;
```

```cpp
	bool resolvePath(const char* p, Ext2Inode& out, int depth) {
		if (depth > 8 || !p || p[0] != '/')
			return false;
		Ext2Inode cur = getInode(2);   // ext2 root inode is #2
		int i = 1;
		while (i < EXT_PATH_SCAN_MAX && p[i]) {
			int j = i;
			while (j < EXT_PATH_SCAN_MAX && p[j] && p[j] != '/')
				j++;
			int len = j - i;
			if (len > 0) {
				Ext2Inode child;
				if (!getChildrenInode(cur, p + i, len, child))
					return false;
				if (isSymlink(child)) {            // follow the link (absolute target only)
					char tgt[256];
					if (!readSymlinkTarget(child, tgt) || tgt[0] != '/')
						return false;
					if (!resolvePath(tgt, child, depth + 1))
						return false;
				}
				cur = child;
			}
			if (j >= EXT_PATH_SCAN_MAX)
				return false;                      // ran into the ceiling: reject, don't wrap
			i = (p[j] == '/') ? j + 1 : j;
		}
		if (i >= EXT_PATH_SCAN_MAX)
			return false;
		out = cur;
		return true;
	}
```

- [ ] **Step 2: `resolveParent`** — bounded skan długości:

```cpp
	bool resolveParent(const char* path, Ext2Inode& parent, int& parentNo, char* name, int& nameLen) {
		if (!path || path[0] != '/') return false;
		int end = 0;
		while (end < EXT_PATH_SCAN_MAX && path[end]) end++;
		if (end >= EXT_PATH_SCAN_MAX) return false;   // unterminated/oversized: reject
		while (end > 0 && path[end - 1] == '/') end--;
		int start = end; while (start > 0 && path[start - 1] != '/') start--;
		nameLen = end - start;
		if (nameLen <= 0 || nameLen > 255) return false;
		for (int i = 0; i < nameLen; i++) name[i] = path[start + i];
		name[nameLen] = 0;
		char pp[256]; int k = 0;
		for (int i = 0; i < start && k < 255; i++) pp[k++] = path[i];
		if (k == 0) pp[k++] = '/';
		pp[k] = 0;
		return resolvePathNum(pp, parent, parentNo, 0);
	}
```

  Sprawdź też bliźniaczy `resolvePathNum` (jeśli ma własny skan — zastosuj ten sam bound;
  jeśli deleguje do `resolvePath`, nic nie trzeba).

- [ ] **Step 3: Zielone.**
  Run: `make test64 2>&1 | tail -15` oraz `make test-san 2>&1 | tail -15`
  Expected: wszystkie testy PASS (w tym `oversized components`), coverage ≥90%, zero
  raportów sanitizerów.
- [ ] **Step 4: Commit.**

```bash
git add fs/ExtFilesystem.h
git commit -m "fs/ext: bound path-resolution scans (EXT_PATH_SCAN_MAX) — no run-off on bad buffers"
```

### Task 5: Reprodukcja -O2 w QEMU (+ bisekcja, jeśli wciąż czerwone)

**Files:**
- Modify: `Makefile:1109-1111` (`image64:` — passthrough `KOPTFLAGS` do wnętrza kontenera)

**Interfaces:**
- Consumes: fixy z Task 3-4.
- Produces: decyzja „zielone → Task 6" albo zlokalizowany dodatkowy winowajca + fix.

- [ ] **Step 1: Passthrough flagi do kontenera.** `image64:` woła `make` W ŚRODKU dockera,
  więc `make image64 KOPTFLAGS=...` dziś NIE dociera do builda jądra. Zmień recipe:

```make
.PHONY: image64
image64:
	$(DOCKER_RUN) make ARCH=x86_64 KOPTFLAGS='$(KOPTFLAGS)' _image64
```

- [ ] **Step 2: Czysty build z -O2 i boot headless.**

```bash
make clean64 2>/dev/null || rm -rf bin/k64
make image64 KOPTFLAGS='-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks'
./scripts/smoke-x86_64.sh    # istniejący smoke: boot do "nanos login:" (bounded poll)
```

  Expected: smoke PASS. Jeśli PASS → przejdź do Step 5.
- [ ] **Step 3 (tylko jeśli triple-fault):** złap miejsce:

```bash
qemu-system-x86_64 -m 512 -drive file=disk/image64-grub2.img,format=raw \
  -display none -serial stdio -d int,cpu_reset -D /tmp/o2-int.log -no-reboot &
sleep 20; pkill -f 'file=disk/image64-grub2.img' || true
grep -m3 -B2 "check_exception.*0xd\|triple" /tmp/o2-int.log   # RIP= w zrzucie
```

  RIP → symbol: `docker run --rm -v $(pwd):/src -w /src <DOCKER_IMAGE> \
  x86_64-elf-addr2line -f -e bin/k64/kernel64.elf <RIP>` (nazwę obrazu wziąć z
  `DOCKER_IMAGE` w Makefile; jeśli link nie zostawia ELF-a, dodać tymczasowo `-Wl,-Map` /
  zachować ELF przed objcopy — patrz reguła `_image64`).
- [ ] **Step 4 (tylko jeśli Step 3 niejednoznaczny): bisekcja per-TU.** W kontenerowej
  części Makefile (pod definicją `OBJECTS`, Makefile:~1290) dodawaj TYMCZASOWO
  target-specific flagi dla połówki obiektów i szukaj połówkami:

```make
# BISECT (temporary): O2 for half the kernel TUs; KOPTFLAGS stays empty globally.
$(KOBJ)Syscall.o $(KOBJ)SyscallDispatch.o $(KOBJ)Exec.o $(KOBJ)Vfs.o: KEXTRA += -O2 -fno-strict-aliasing -fno-delete-null-pointer-checks
```

  Po każdej zmianie: `make image64 && ./scripts/smoke-x86_64.sh`. Zawęź do jednego TU,
  napraw właściwy defekt (wracając do wzorca Task 2-4: najpierw czerwony test hostowy,
  potem fix), usuń linie bisekcji, wróć do Step 2.
- [ ] **Step 5: Commit passthrough.**

```bash
git add Makefile
git commit -m "build: image64 passes KOPTFLAGS through to the in-container kernel build"
```

### Task 6: Trwałe włączenie -O2 + aktualizacja komentarza

**Files:**
- Modify: `Makefile:1250-1267` (`KOPTFLAGS` + blok komentarza)

**Interfaces:**
- Produces: kernel domyślnie `-O2`; komentarz odzwierciedla stan faktyczny.

- [ ] **Step 1: Flip.** Zamień `KOPTFLAGS=` (Makefile:1267) na:

```make
# KERNEL at -O2 since 2026-07: the ext path-resolution UB that used to triple-fault the
# boot-time root mount is fixed (bounded scans in ExtFilesystem, non-mutating String
# operator+, null-safe String view — see plans/2026-07-04-ext-path-ub-kernel-o2.md).
# Same two safety flags as userland, for the same free-pointer-cast reasons.
KOPTFLAGS=-O2 -fno-strict-aliasing -fno-delete-null-pointer-checks
```

  I usuń stary czterolinijkowy komentarz „KERNEL is held at -O0 for now..." (Makefile:1263-1266).
- [ ] **Step 2: Pełny czysty build + cała bramka.**

```bash
rm -rf bin/k64 && make image64
make test64
make verify64
```

  Expected: build czysty, testy + coverage PASS, wszystkie 16 smoke'ów verify64 PASS.
  Uwaga: verify64 trwa długo (seryjne QEMU) — odpal w tle i POLLUJ z deadlinem
  (bounded), nie czekaj biernie.
- [ ] **Step 3 (opcjonalny pomiar):** zanotuj w tym pliku czas bootu do `nanos login:`
  przed/po (z logu smoke-x86_64) — to jest „~10x kernel win" z komentarza; warto mieć liczbę.
- [ ] **Step 4: Commit.**

```bash
git add Makefile
git commit -m "kernel: enable -O2 (+no-strict-aliasing, +no-delete-null-pointer-checks)"
```

### Task 7: Sanity na Dellu + PR

**Files:** brak zmian kodu.

- [ ] **Step 1: Obraz USB + boot na Dell Latitude 5310** (flow jak przy poprzednich testach
  real-HW: nagrać `disk/image64-grub2.img` na pendrive, boot z USB). Sanity: login `jan`,
  `ls /`, `cat /hello.txt` lub odczyt dowolnego pliku z ext-root, `git status` w repo
  testowym jeśli jest na obrazie. Ext lookupy to gorąca ścieżka — realny sprzęt weryfikuje
  -O2 poza TCG.
- [ ] **Step 2: PR.** `gh pr create` z gałęzi `fix/ext-path-ub-o2` na `main`; opis: co było
  (UB + -O0), co jest (bounded scans, String semantics, -O2), dowody (test-san, verify64,
  boot Dell). Bez stopki AI.
- [ ] **Step 3:** Po merge zaktualizuj `docs/superpowers/plans/2026-07-04-tech-debt-priorities.md`
  (odhacz P0-B) i `docs/superpowers/plans/2026-06-11-gui-performance.md` (follow-up „kernel -O2"
  wykonany).

---

## Self-review (wykonany przy pisaniu)

- **Pokrycie:** wszystkie trzy zidentyfikowane defekty mają task+test (T3: operator+/null,
  T4: bounded scans); przypadek „winowajca gdzie indziej" ma ścieżkę T1 (sanitizery) i T5
  Step 3-4 (bisekcja per-TU przez `KEXTRA`). Flip flagi (T6) bramkowany pełnym verify64
  i realnym sprzętem (T7).
- **Placeholdery:** brak — każdy krok ma kod/komendę; jedyne odwołania do „istniejących
  wzorców" wskazują konkretny plik:linia (`test_ext2.cpp:15-25`).
- **Spójność typów:** `cstr()` używany w `indexOf/compareTo/operator char*` zdefiniowany
  w T3 Step 1; `EXT_PATH_SCAN_MAX` z T4 Step 1 używany w obu metodach; sygnatury `stat(String, FileStat&)`
  zgodne z `tests/test_ext2.cpp:58`.
- **Ryzyka nazwane:** zmiana `operator+` na niemutujący może zmienić zachowanie call-site'u,
  który (nieświadomie) POLEGAŁ na mutacji — łapane przez pełne `make test64` + verify64 w T6;
  domyślny konstruktor celowo NIE alokuje (static-init przed heapem).
