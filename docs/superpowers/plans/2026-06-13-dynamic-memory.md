# Plan: dynamiczna pamięć userlandu (jak na Linuksie)

**Cel:** aplikacja ma móc wykorzystać **cały dostępny RAM**, a nie sztywną, zarezerwowaną pulę
(dziś heap = 64 MiB, mmap = 64 MiB). Docelowo semantyka zbliżona do Linuksa: duża przestrzeń VA,
alokacja na żądanie, limit = fizyczny RAM (potem ENOMEM), bez twardych okien.

---

## 1. Diagnoza — gdzie faktycznie jest limit

Stan na dziś (`arch/x86/mm/mmu_x86.cpp`):

```
NX_MOD_BASE  = 0x40000000 (1 GiB)   .. +128 MiB   (32 sloty .ndl)
NX_BRK_BASE  = 0x48000000           .. +64 MiB    <-- heap (brk/sbrk) zakap. na 64 MiB
NX_MMAP_BASE = 0x50000000           .. +64 MiB    <-- mmap zakap. na 64 MiB
user window  = 0x800000 (4 MiB) + 512 KiB stack na górze
```

Fakty (zweryfikowane w kodzie):

1. **Fizyczny RAM nie jest limitem.** `mmuSetUserBrk`/`mmuMapAnon` alokują ramki przez
   `g_fa->alloc()` — globalny `FrameAllocator` pokrywający **cały** usable RAM (`bootMemTop`).
   Ramki są przydzielane na żądanie, z jednej puli dla wszystkich procesów.
2. **Limit = okna VA.** Dispatch `brk` klampuje break do `[brkBase, brkMax]`; `SyscallDispatch.cpp`
   przy `mmap2` zwraca `-ENOMEM` gdy `mmapNext + bytes > mmuMmapMax()`. Czyli heap nie urośnie ponad
   64 MiB, a mmap ponad 64 MiB — **niezależnie od tego, ile jest RAM-u**. To jest ta „zarezerwowana
   pula ~30 MB", którą czuje user.
3. **Okien nie da się naiwnie powiększyć.** `copyUserWindowFrom` (fork) i `freeUserWindow` (exit)
   iterują **cały zadeklarowany zakres okna** krokiem 4 MiB (PDE): `for (va = BASE; va < MAX; va +=
   4 MiB)`. Powiększenie okna do np. 3 GiB = setki iteracji PDE na każdy fork/exit, nawet dla pustego
   okna. To trzeba naprawić, zanim okna urosną.
4. **Mapowanie jest eager (zachłanne).** `brk`/`mmap` od razu alokują i zerują ramki, pod CR3 jądra
   (bo katalog procesu nie identity-mapuje całego RAM-u — „kernel-CR3 trap"). Brak demand-paging.
5. **#PF handler jest tylko debugowy** (`fault_x86.cpp`, wektor 14): drukuje CR2 i faultuje/panikuje.
   Nie robi fault-in. To jest miejsce, w które wpięlibyśmy lazy alokację.
6. **Brak rozdziału kernel/user VA.** Jądro jest identity-mapowane w niskich adresach i współdzielone
   w każdym katalogu procesu; user window siedzi nisko (`0x800000`). Działa, ale jest „ciasno" i
   wymusza kernel-CR3 trap przy każdej alokacji userland.

**Wniosek:** żeby apka użyła całego RAM, trzeba (a) zdjąć/rozszerzyć kapy okien i (b) sprawić, by
fork/exit/copy nie płaciły za rozmiar okna, tylko za faktycznie zmapowane strony. Pełna „linuksowość"
(overcommit, sparse VA) to dodatkowo demand-paging + VMA.

---

## 2. Decyzje projektowe (i rekomendacja)

| Kwestia | Opcje | Rekomendacja |
|---|---|---|
| Jak duże okna user VA | (a) większe stałe; (b) jedno wielkie okno user „od X do kernela" | docelowo (b): jedna duża arena userlandu |
| Eager vs demand paging | (a) eager bez kapów; (b) demand-paging (fault-in w #PF) | (a) jako szybki efekt, (b) jako cel właściwy |
| Rozdział kernel/user | zostawić low identity; albo high-half kernel (3G/1G) | high-half opcjonalnie, faza 3 (duży refaktor) |
| Overcommit | nie / tak | tak, naturalnie wychodzi z demand-paging |
| Limit twardy | brak / `RLIMIT_AS` / tylko RAM | tylko fizyczny RAM (+ ewentualny per-proc cap konfigurowalny) |

Ograniczenie nieusuwalne: to **32-bit x86**, więc cała VA procesu = 4 GiB, z czego część zajmuje
jądro. Realny pułap user VA ≈ 3 GiB (przy high-half) lub mniej (obecny układ). „Cały RAM" działa,
dopóki RAM ≤ rozmiarowi okna user; przy >~3 GiB RAM ogranicza nas 32-bit, nie nasz kod (to oczekiwane
na i686; PAE/64-bit poza zakresem).

---

## 3. Plan fazowy

### Faza 0 — pomiar i siatka bezpieczeństwa (przygotowanie)
- Dodać test/program userland (`memhog`): `mmap`/`sbrk` w pętli aż do błędu, raportuje ile MiB udało
  się zmapować i zapisać (dotknąć każdą stronę). To jest „przed/po" benchmark celu.
- Potwierdzić w `/proc/meminfo`, że `MemFree` spada zgodnie z alokacją (czyli ramki realnie schodzą z
  globalnej puli).
- Test hosta (`test_addressspace.cpp`): dodać przypadki na rzadkie (sparse) mapowanie i na to, że
  teardown zwalnia tylko zmapowane PDE.

### Faza 1 — zdjąć kapy + płacić tylko za zmapowane strony (szybki, realny efekt)
To samo w sobie spełnia cel użytkownika („apka używa całego RAM").

1. **`AddressSpace`: iterować tylko obecne PDE.** Przerobić `copyUserWindowFrom`/`freeUserWindow` tak,
   by przechodziły katalog stron i obsługiwały **tylko PDE z bitem present** w danym zakresie, zamiast
   pętli `for va in [BASE,MAX)` krokiem 4 MiB. (W `mmu_x86.cpp` `mmuCopyAddressSpace`/
   `mmuFreeAddressSpace` wołają je dla każdego okna — po zmianie koszt = liczba realnie zmapowanych
   stron, nie rozmiar okna.)
2. **Rozszerzyć okna.** Podnieść `NX_BRK_MAX` i `NX_MMAP_MAX` tak, by user VA sięgała blisko granicy
   jądra (np. heap i mmap dostają po ~1 GiB, albo jedna wspólna arena). Dobrać układ tak, by nie kolidował
   z module band (`0x40000000`) i framebufferem.
3. **brk/mmap: limit = brak ramek, nie kap.** `mmuSetUserBrk` już zwraca `-1` gdy `g_fa->alloc()`
   pada; dispatch `brk` ma to mapować na „break bez zmian" (glibc wykrywa porażkę), a `mmap2` na
   `-ENOMEM`. Usunąć/rozluźnić sztuczny klamp do `mmapMax`/`brkMax` (zostaje tylko granica areny VA).
4. **`/proc/meminfo`/`/proc/self`** — upewnić się, że raportują realny stan (MemFree z frame
   allocatora; opcjonalnie VmSize/VmRSS per-proces).
5. **Test:** `memhog` w QEMU z różnym `-m` (np. 256 MiB, 1 GiB) — apka ma dojść ~do MemFree, potem
   czysty `-ENOMEM` (bez triple-faulta). Fork procesu z dużym heapem ma być szybki (dowód, że pętle
   nie iterują pustki).

**Definicja ukończenia Fazy 1:** program userland alokuje i zapisuje pamięć aż do wyczerpania
fizycznego RAM (minus jądro), a nie zatrzymuje się na 64 MiB; fork/exit nie spowalniają z rozmiarem
okna.

### Faza 2 — demand paging + VMA (właściwa semantyka Linuksa: overcommit, sparse)
1. **Per-proces lista VMA** (`Process`): tablica/rejestr przedziałów `[start,end) + prot + flags`
   (anon/file, RW). `mmap` *rezerwuje* VMA (tani, bez ramek); `brk` to specjalna VMA heap.
2. **Realny #PF handler.** W `fault_x86.cpp`/MI: na #PF sprawdź CR2 w VMA bieżącego procesu →
   jeśli trafia w prawidłową, zapisywalną VMA: zaalokuj ramkę, wyzeruj, zmapuj, wróć (fault-in).
   W przeciwnym razie `SIGSEGV` do procesu (a nie panic). Obsłużyć kod błędu (present/write/user).
3. **mmap/brk leniwe.** Mapowanie staje się rezerwacją VA; ramki dochodzą przy pierwszym dotknięciu.
   To daje **overcommit** i natychmiastowy `mmap` wielkich obszarów.
4. **fork:** copy-on-write zamiast eager copy (oznacz strony RO + bit CoW; #PF na zapis duplikuje
   stronę). Duży skok wydajności fork/exec (dziś eager-kopiuje całe okno).
5. **Teardown** chodzi po VMA (a katalog stron tylko po obecnych PDE).
6. **Testy:** `mmap` 1 GiB na 256 MiB RAM (overcommit OK, zapis ponad RAM → OOM/SIGSEGV w zdefiniowany
   sposób); CoW fork (rodzic/dziecko nie widzą swoich zapisów); host-testy VMA + lookup.

### Faza 3 — (opcjonalna, duży refaktor) rozdział high-half kernel (3G/1G)
- Przenieść jądro do `0xC0000000+` (high-half), userland dostaje ciągłe `0..0xBFFFFFFF` (~3 GiB).
  Usuwa „kernel-CR3 trap" (jądro zawsze zmapowane, alokacja/zerowanie ramek bez przełączania CR3),
  daje czystą, dużą, ciągłą arenę userlandu i upraszcza VMA.
- Dotyka: `linker.ld` (baza), `loader.s` (wczesne mapowanie high-half przed skokiem), cały kod jądra
  działający dziś pod identity-low, `mmuInitKernel`, `adoptKernelDirectory`. Kosztowne i ryzykowne —
  robić tylko jeśli Fazy 1–2 okażą się ciasne.

---

## 4. Ryzyka i pułapki
- **32-bit ceiling:** >~3 GiB RAM nieosiągalne dla pojedynczego procesu bez PAE/64-bit (poza zakresem).
- **Kernel-CR3 trap:** dopóki nie ma high-half (Faza 3), alokacja/zerowanie ramek wymaga przełączenia
  na CR3 jądra — #PF handler musi to robić poprawnie i tanio (albo identity-mapować całość w katalogu
  procesu, co marnuje PDE). Uważać na re-entrancy #PF.
- **TLB/CR3:** po zmianie PTE trzeba flush (dziś robi to reload CR3); przy demand-paging używać
  `invlpg` dla pojedynczej strony zamiast pełnego reloadu.
- **Brak swapu:** OOM = twardy; zdefiniować zachowanie (ENOMEM dla mmap/brk; SIGSEGV/OOM-kill przy
  dotknięciu overcommitowanej strony bez ramek). Bez OOM-killera — najpierw prosty SIGSEGV/abort.
- **Stos userland** (dziś stały 512 KiB) — rozważyć auto-grow stosu przez #PF (guard page) w Fazie 2.
- **Regresje:** istniejące apki (doom, vim, nwm — duże mmapy/heap) muszą działać; `make test`
  (host AddressSpace/FrameAllocator/Heap) + przebieg QEMU dla GUI/portów jako bramka.

---

## 5. Kolejność prac (rekomendacja)
1. **Faza 0** (memhog + pomiar) — 0,5 dnia.
2. **Faza 1** (present-PDE walk + zdjęcie kapów) — realizuje cel użytkownika; mała, bezpieczna zmiana
   skupiona w `arch/x86/mm/{AddressSpace,mmu_x86}.cpp` + dispatch. **Tu bym celował na start.**
3. **Faza 2** (demand-paging + VMA + CoW) — właściwa linuksowość; większa, ale dobrze izolowana
   (#PF handler + VMA w `Process`).
4. **Faza 3** (high-half) — tylko jeśli potrzebna.

Po Fazie 1 user dostaje to, o co prosił (apka = cały RAM). Faza 2 dokłada overcommit, szybki fork i
sparse mmap. Aktualizacja dokumentacji: `docs/{en,pl}/memory.md` po każdej fazie (zmiana mapy pamięci
i modelu alokacji).
