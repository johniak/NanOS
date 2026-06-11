# Audyt schedulera: znaleziska + plan naprawy

> **STATUS: UKOŃCZONO (wszystkie fazy 1–7).** Każda faza = host-testy + build + weryfikacja
> QEMU + osobny commit na gałęzi `dockerized-build`:
> - Faza 1 `dd3d558` — A1 strażnik `wake()`, A5 (BLOCKED przed gotowym `kesp`), A8 reap DONE-kthreadów
> - Faza 2 `f7f5c76` — A2 re-parenting sierot do init + SIGCHLD przy exit
> - Faza 3 `a75e326` — A3 malloc precz ze ścieżki IRQ + `Scheduler::resume()` (SIGCONT osobno od `wake`)
> - Faza 4 `ebb949f` — P2 (usunięty clock-kthread), P3 (warunkowy CR3), P4 (O(1) `task→proc`)
> - Faza 5 `23c4ec5` — A6 sekcja krytyczna `cli` w `schedule()` (`cpuIrqSave/Restore`)
> - Faza 6a `585dabf` — P5 kwant 10 ms, P6 deadline-sleep (`nanosleep`)
> - Faza 6b `c04304c` — P1 + A7 wait-queues (pipe/pty/konsola, event-driven)
> - Faza 7 `08f98dc` — A4 `sti` w syscallach + prepare-to-wait w prymitywach uśpienia
> Świadomie pominięte (mniejszy zysk, większe ryzyko): event-driven `poll()` na wielu kolejkach
> (wymaga oczekiwania zadania na >1 kolejce naraz) — `poll` nadal odpytuje co tyk (poprawnie).


Audyt `kernel/Scheduler.{h,cpp}`, `arch/x86/cpu/{sched_x86.cpp,switch.S,irq.S,isr.S}`,
ścieżek blokujących w `kernel/SyscallDispatch.cpp`, `kernel/Exec.cpp` (exit/wait/sygnały)
i `arch/x86/drivers/input_x86.cpp`. **Nic nie zmieniono — to jest plan.**

## Inwarianty, których NIE wolno naruszyć

- **Deferred preemption zostaje**: przełączenie zadania wykonuje się TYLKO w bezpiecznych
  punktach — na powrocie z IRQ do ring 3 (`schedPreempt` w `irq.S`, pomijany przy powrocie
  do ring 0) albo przy dobrowolnym yield/block w jądrze. Żadnego przełączania w handlerze
  IRQ i żadnego busy-wait w idle (`halt_or_hlt` = `sti; hlt` zostaje).
- Idle = slot 0, wybierany tylko gdy nic innego nie jest runnable (`pickNext`).
- Czyste rdzenie (`nextRunnable`, `loadDecay`) pozostają host-testowane; nowa logika
  (wait-queue, kwant, deadline'y) ma wejść do testów tak samo.

## Ustalony stan faktyczny (na czym opierają się znaleziska)

- Bramka syscalli `0xEE` = **interrupt gate** (`Idt.cpp:72`) + `cli` w `isr128` → **cały
  syscall biegnie z IF=0**; nic w ścieżce syscalla nie robi `sti` aż do `iret`.
- Kthready startują przez `taskTrampoline`, który robi `sti` (`switch.S:40`) → **kod jądra
  w kthreadach (np. `execProgram` z `initTaskBody`) biegnie z IF=1** i woła
  `ioWait()`/`schedule()` z włączonymi przerwaniami. Idle też woła `schedule()` z IF=1.
- `free()` jest prawdziwy (Heap z koalescencją, `mm/memory_manager.cpp`) — recykling
  stosów jądra w `reap()` działa. Heap **nie ma żadnej ochrony przed reentrancją z IRQ**.
- `ProcTable::MAX = 1024` → `MAXTASKS = 1032`.
- **Nie ma re-parentingu sierot do init** — `procExit` (`Exec.cpp`) robi tylko
  `orphanCheckOnExit` (SIGHUP/SIGCONT dla osieroconych grup zatrzymanych); pole
  `parent` umarłego rodzica zostaje.

---

## A. Błędy poprawności (od najpoważniejszego)

### A1. `Scheduler::wake()` bez strażnika stanu — wskrzeszenie ZOMBIE

`wake()` (`Scheduler.cpp:191`) ustawia `READY` **bezwarunkowo**. `procExit`
(`Exec.cpp:~243`) budzi rodzica bez sprawdzenia jego stanu; tak samo ścieżki SIGCONT
(`Exec.cpp:380,534`) budzą rodzica zatrzymanego dziecka. Ponieważ **nie ma re-parentingu
(A2)**, scenariusz jest osiągalny: proces A forkuje B i kończy się bez `waitpid` (A =
zombie, bo jego własny rodzic go jeszcze nie zebrał); gdy potem B robi exit, `procExit`
znajduje `byPid(B->parent)` = zombie-A i `wake(A->task)` przerzuca **TASK_ZOMBIE →
TASK_READY**. Scheduler wznawia zadanie-zombie za jego finalnym `schedule()` w `procExit`
→ wpada w `for(;;){}` jako wiecznie runnable task: zjada CPU na zawsze (z już zwolnioną
przestrzenią adresową; kstack wciąż żywy, więc "działa", ale system trwale muli).

**Fix:** w `wake()` budzić wyłącznie `TASK_BLOCKED` (przejście BLOCKED→READY; READY/RUNNING
= no-op, ZOMBIE/DONE/STOPPED/FREE = nietykalne). Jeden strażnik w jednym miejscu zamiast
poprawiania wszystkich wywołań (część już ma własne strażniki — `signalSend` sprawdza
`state == TASK_BLOCKED`, reszta nie).

**Test (host):** symulacja tablicy stanów — wake na ZOMBIE/DONE/STOPPED nie zmienia stanu;
wake na BLOCKED daje READY. **Test (QEMU):** `sh -c 'program_forkujacy_i_wychodzacy &'`,
potem exit dziecka — brak 100% CPU w `/proc`, brak ducha w `ps`.

### A2. Brak re-parentingu do init → wieczne zombie (leak slotów + 8 KB kstack)

Gdy rodzic umiera przed dzieckiem, dziecka **nikt nigdy nie zbierze**: `waitpid` woła tylko
rodzic po `parent`-pid, a ten pid już nie żyje (i nigdy nie zostanie ponownie użyty —
`g_nextPid` rośnie monotonicznie). Każde takie dziecko po swoim exit zostaje na zawsze:
slot `Process` (1024 max), slot `Task` + **8 KB kstack** (zwalniane dopiero w
`waitProcess` → `Scheduler::reap`). Długo żyjący system (bash + potoki + demony) wyczerpie
tablicę → `fork` = -EAGAIN.

**Fix:** w `procExit`, zanim umrze, przepisać `parent` wszystkich swoich dzieci na pid 1
(init) i posłać init SIGCHLD/wake, a init (`user/init`) musi pętlić `waitpid(-1, …,
WNOHANG)` po SIGCHLD (sprawdzić, czy już to robi; jeśli nie — dopisać). To przy okazji
likwiduje osiągalność A1 (rodzic-zombie przestaje istnieć jako cel wake), ale strażnik
A1 i tak wchodzi (defense in depth).

**Test (host):** `reapChild`/re-parenting na czystej tablicy procesów. **Test (QEMU):**
pętla `sh -c 'true &'` ×200 → liczba procesów w `/proc` wraca do bazy, `fork` nie zwraca
-EAGAIN.

### A3. `malloc` w kontekście IRQ → możliwa korupcja sterty (już dziś, bez zmian w IF)

Ścieżka: klawiatura IRQ1 → `inputFeedScancode` → callback cooked-mode
(`input_x86.cpp:74-76`) → `consoleSignal(SIGINT/QUIT/TSTP)` → `signalSendGroup`
(`Exec.cpp:~520`) → **`malloc(sizeof(int)*1024)`**; podobnie `signalSend(-1)` →
`malloc(sizeof(ProcInfo)*1024)`. Heap nie jest reentrancyjny. Kolizja: kthread biegnie z
IF=1 (patrz „stan faktyczny") i właśnie jest w środku `malloc` (np. `execProgram` ładuje
init.nxe), użytkownik wciska Ctrl+C → IRQ wchodzi w środek operacji na free-liście →
korupcja. Okno wąskie (boot, kthready), ale realne — i stanie się ogromne, jeśli
kiedykolwiek włączymy IF w syscallach (A4).

**Fix (wybrać pierwszy):**
1. **Usunąć malloc ze ścieżki IRQ**: `signalSendGroup` ma już fallback na bufor stosowy
   `int stackpids[64]` — uczynić go ścieżką jedyną (64 to aż nadto na grupę procesów;
   jawny `log`/licznik gdy grupa większa). Analogicznie `signalSend(-1)` — iterować
   po tablicy bezpośrednio zamiast snapshotu na stercie (to wewnątrz jądra, wolno).
2. Dodatkowo (tanio, higienicznie): w `Heap::alloc/free/realloc` sekcja krytyczna
   `pushf; cli` … `popf` — chroni przed KAŻDĄ przyszłą reentrancją z IRQ.

**Test (host):** `signalSendGroup` z grupą >64 — poprawny rezultat bez sterty.
**Test (QEMU):** trzymanie Ctrl+C podczas bootu/exec — brak `v=0d/0e` w logu `-d int`.

### A4. Gubione tyki zegara podczas długich syscalli (IF=0) → dryf czasu

Cały syscall biegnie z IF=0, a PIC trzyma tylko **jeden** zaległy IRQ0. Odczyt dużego
pliku (`SYS_read` → ext → ATA PIO, jedna komenda na sektor, polling DRQ) potrafi trwać
wiele ms z wyłączonymi przerwaniami → tyki się sklejają: `g_ticks` (= jedyny zegar:
uptime, `clock_gettime`, timeouty `poll`, `nanosleep`, kwanty) płynie wolniej niż czas
rzeczywisty pod obciążeniem I/O. To samo opóźnia wejście (klawiatura czeka na EOI… 
faktycznie na IF).

**Fix docelowy (osobna faza, po A1-A3 i A5-A6):** `sti` na początku `kernelSyscall`
(po prologu, gdy segmenty/rejestry już zdjęte). Deferred preemption to **dopuszcza** —
`irq.S` i tak pomija `schedPreempt` przy powrocie do ring 0, więc inwariant „jądro nigdy
nie jest przełączane w dowolnym miejscu" stoi. Wymagania wstępne (stąd kolejność):
- heap odporny na IRQ (A3),
- `allocSlot` nie publikuje slotu READY przed gotowym `kesp` (A5),
- wzorzec prepare-to-wait tam, gdzie test-warunku i `block()` przestają być atomowe
  (A6): `inputRead` (test `rawEmpty` → IRQ może obudzić ZANIM uśniemy → lost wakeup)
  i `waitProcess` (dziś chroni IF=0; po `sti` dziecko nadal nie może się wcisnąć —
  ring-0 bez preempcji — ale IRQ-wake już tak).
**Fix minimalny, jeśli faza odpada:** zaakceptować dryf, ale udokumentować go przy
`/proc/uptime` i timeoutach.

**Test (QEMU):** `time cat duzy-plik > /dev/null` w bash vs `sleep` — uptime przed/po
porównany z czasem hosta (skrypt QEMU); po fixie rozjazd ≈ 0.

### A5. `allocSlot` publikuje slot jako `TASK_READY` z niezainicjowanym `kesp`

`allocSlot` (`Scheduler.cpp:78`) ustawia `state = TASK_READY` od razu; przy `fork`
(`createBlank` → `archForkChild` w `Exec.cpp:~181-194`) `kesp` powstaje **później**.
Dziś nieosiągalne (fork w syscallu z IF=0, jądro się nie przełącza), ale to mina pod A4
i pod każdy przyszły kod, który blokuje między tymi punktami: `pickNext` mógłby wybrać
slot ze śmieciowym `kesp` → skok w śmieci.

**Fix:** `allocSlot` ustawia `TASK_BLOCKED` (albo nowy `TASK_INIT`); na `READY`
przechodzi jawnie: `create()` po `archTaskBootstrap`, `forkProcess` po `archForkChild`
(ten drugi już to robi — `Exec.cpp:194`). Zmiana dwulinijkowa, czysto MI, host-testowalna.

### A6. `schedule()` wołany z IF=1 (kthready, idle) — okna wyścigu z `onTick`/`wake`

`taskTrampoline` robi `sti`, więc `ioWait()`/`schedule()` z kthreadów oraz `schedule()`
z `idleBody` biegną z włączonymi przerwaniami. `onTick` (IRQ) w środku `schedule()`:
zaksięguje tyk na wpół-przełączony `g_cur` (kosmetyka), przeskanuje `g_tasks` w trakcie
mutacji stanów (dziś łagodne — IRQ nie przełącza). To działa „przypadkiem"; po A4 te
same okna otwierają się dla wszystkich syscalli.

**Fix:** sekcja krytyczna w `schedule()`: `pushf; cli` na wejściu, przywrócenie flag
poprzez kontekst (nowo wznowione zadanie i tak odtwarza swoje IF przez `iret`/`sti`
trampoliny; po powrocie z `archContextSwitch` przywrócić zapamiętane flagi `popf`).
Analogicznie krótkie `cli` wokół `ioWait`/`block` (para ustaw-stan + schedule).

### A7. Pojedynczy `g_inputWaiter` — drugi czytelnik konsoli nadpisuje pierwszego

`input_x86.cpp:99,116`: globalny wskaźnik jednego oczekującego. Dwa procesy blokujące
read na konsoli → pierwszy nigdy nie zostanie obudzony przez IRQ (śpi do sygnału).
W praktyce osłonięte przez SIGTTIN (grupa tła dostaje sygnał zamiast czytać), ale to
poleganie na polityce tty, nie na poprawności mechanizmu.

**Fix:** rozwiązuje się naturalnie w P1 (wait-queue na urządzeniu wejścia zamiast
jednego wskaźnika). Nie ruszać osobno.

### A8. Kthread, którego `body` wraca → `TASK_DONE` bez reapa (leak 8 KB + slot)

`runCurrentBody` (`Scheduler.cpp:216`) zostawia DONE na zawsze; nikt nie woła `reap()`
dla kthreadów. Dziś dotyczy tylko `initTaskBody` po nieudanym exec (i tak system bez
init jest martwy). Niski priorytet: dopisać reap zadań DONE w `schedule()`/`onTick`
(leniwie, gdy `g_cur != slot`) albo zostawić z komentarzem — decyzja przy implementacji.

---

## B. Wydajność (od największego zysku)

### P1. Tick-polling zamiast event-driven wake — systemowy koszt nr 1

`ioWait()` = „obudź mnie na NASTĘPNYM tyku" (`wantTick`), a `onTick` budzi **wszystkich**
takich czekających **co 1 ms**. Każdy zablokowany read/write na pipe/pty, każdy `poll`
i `nanosleep` generuje ~1000 cykli budzenie→przełączenie→retest-warunku→blokada/s,
każde z pełnym context switchem (i flushem TLB, patrz P3). Pipe/PTY **nie budzą**
czytelnika przy write ani pisarza przy read — wszystko jest 1-ms pollingiem. Przy nadchodzącym
NanWM (kompozytor + klienci na potokach) to się skaluje w dziesiątki tysięcy pustych
przełączeń/s. Klawiatura już ma właściwy wzorzec: `block()` + `wake()` z IRQ.

**Fix (zachowuje deferred preemption — `wake` tylko ustawia READY+flagę, jak dziś):**
- **Wait-queue** (lista `Task*`) per `Pipe`, per koniec PTY i dla urządzenia wejścia
  (A7). Czysta struktura MI (jak `Pipe`) → host-testy.
- `Pipe::write`/`close` → wake czytelników; `Pipe::read`/`close` → wake pisarzy
  (wywoływane z dispatchu syscalla, czyli z bezpiecznego kontekstu).
- Pętle w `SyscallDispatch.cpp:162,176` (read/write): zamiast `ioWait()` → `sleepOn(q)`.
- `poll`: śpij na wait-queue **wszystkich** obserwowanych fd + deadline (P6); budzony
  przez wake z dowolnego z nich albo po terminie.
- `wantTick`/budzenie-co-tyk zostaje WYŁĄCZNIE dla czekań czasowych — patrz P6.

**Metryka:** `/proc/stat` `ctxt` na bezczynnym systemie z shellami na PTY: oczekiwany
spadek z tysięcy/s do dziesiątek/s.

### P2. `clockTaskBody` budzi się 1000×/s, żeby zwiększyć licznik

`Kernel.cpp:113-117`: kthread istnieje tylko po to, by `g_bgwork++` co tyk (dowód
żywotności schedulera z wczesnego etapu). To 2000 przełączeń kontekstu/s na pusto.
**Fix:** usunąć kthread (uptime liczy się z `Scheduler::ticks()`); jeśli dowód
żywotności ma zostać — niech śpi na deadline 1 s (po P6). Najtańsza pozycja z listy.

### P3. Bezwarunkowy `mov cr3` w `archContextSwitch` → pełny flush TLB co przełączenie

`switch.S:30`: każdy switch przeładowuje CR3, także idle↔task i kthread↔kthread, które
dzielą katalog jądra. Przy dzisiejszym tick-pollingu (tysiące przełączeń/s) płacimy
ciągłe zimne TLB. **Fix:** porównać zapisane CR3 nowego zadania z bieżącym
(`mov eax, cr3; cmp …; je .skip`) i pominąć przeładowanie, gdy równe. 3 instrukcje
w asm, zero zmian MI. (Po P1 liczba przełączeń spada, ale fix zostaje wart swojej ceny —
user↔idle↔user to wciąż najczęstsza para.)

### P4. `ProcTable::byTask` = liniowy skan 1024 slotów przy KAŻDYM context switchu

`schedule()` (`Scheduler.cpp:129`) woła `ProcTable::byTask`, które porównuje wskaźnik
w pętli po całej tablicy (`Process.cpp:71`). Tysiące switchów/s × 1024 iteracje.
**Fix:** back-pointer `Process*` w `Task` (ustawiany tam, gdzie dziś `p->task = t`),
`schedule()` czyta pole zamiast skanować. Przy okazji: `byPid` (też O(1024), wołane
m.in. w `procExit`, sygnałach, setpgid) może dostać trywialny cache „ostatnio
znaleziony pid" — opcjonalnie, niski priorytet.

### P5. `g_needResched = true` na KAŻDYM tyku → round-robin co 1 ms

`onTick` (`Scheduler.cpp:159`) flaguje reschedule bezwarunkowo, więc dwa CPU-bound
procesy przełączają się 1000×/s (2× flush TLB/ms przed P3). Kwant 1 ms jest poniżej
sensownego minimum. **Fix:** kwant planisty (np. 10 ms = licznik w `onTick`);
`g_needResched` ustawiane gdy (a) kwant bieżącego zadania minął, (b) `wake()` obudził
kogoś (już tak robi — `Scheduler.cpp:194`), (c) tick obudził czekających `wantTick`.
Czysta logika → host-test (sekwencja tyków/wake'ów → oczekiwane punkty przełączeń).

### P6. Czekania czasowe budzą się co tyk zamiast po terminie

`nanosleep` (`SyscallDispatch.cpp:368-388`) i timeout `poll` budzą proces co 1 ms tylko
po to, by porównać `ticks()` z deadlinem — `sleep 5` to 5000 pustych przełączeń.
**Fix:** zamiast `bool wantTick` → pole `unsigned wakeAt` (0 = brak); `onTick` budzi
tylko zadania z `wakeAt <= g_ticks` (arytmetyka z wrapem: `(int)(g_ticks - wakeAt) >= 0`).
Skan w `onTick` zostaje O(n), ale bez context switchów. `ioWait()` zostaje jako
`sleepUntil(ticks()+1)` dla nielicznych miejsc, których nie pokryje P1.

---

## C. Kolejność wykonania (fazy; każda: testy hosta + build + weryfikacja QEMU + commit)

Zasada: najpierw poprawność o małym zasięgu, potem tanie perf, potem przebudowa czekań,
na końcu — opcjonalnie — otwarcie IF w syscallach. Każda faza zostawia system w pełni
działający; A4 jest ostatnie, bo zależy od A3+A5+A6 i zmienia założenia globalnie.

1. **Strażnik `wake()` + porządek stanów** — A1 + A5 (+ ewentualnie A8 przy okazji).
   Mała, czysto MI zmiana; host-testy przejść stanów. QEMU: scenariusz zombie z A1.
2. **Re-parenting do init + zbieranie sierot** — A2 (init: pętla `waitpid(WNOHANG)`
   po SIGCHLD). QEMU: pętla forków-bez-wait, tablica procesów wraca do bazy.
3. **Heap/IRQ** — A3 (malloc precz ze ścieżki sygnałów z IRQ; `pushf;cli` guard w Heap).
   QEMU: Ctrl+C-spam podczas bootu i pod obciążeniem.
4. **Tanie perf** — P2 (usunięcie clock-kthreada), P3 (warunkowy CR3), P4 (back-pointer
   Task→Process). Metryka: `ctxt`/s i czas `make`-podobnego obciążenia w bash przed/po.
5. **`cli` w sekcji krytycznej `schedule()`/`ioWait`/`block`** — A6. Przygotowuje grunt
   pod 6 i 7.
6. **Wait-queues + deadline'y** — P1 + P6 + A7, potem P5 (kwant). Największa faza;
   wait-queue jako czysty moduł MI w `TEST_MODULES` (≥90%), wpięcie w Pipe/Pty/input,
   przełączenie pętli w `SyscallDispatch`. Metryka: `ctxt` na idle ≪ 100/s; interakcja
   (klawiatura, potoki bash) bez regresji opóźnień.
7. **(Opcjonalnie, osobna decyzja) IF=1 w syscallach** — A4, z prepare-to-wait w
   `inputRead`/`waitProcess`. Wymaga pełnego re-audytu ścieżek blokujących z faz 1-6.
   Jeśli faza odpada: udokumentować dryf zegara jako znane ograniczenie.

## D. Weryfikacja całości (po fazach 1-6)

- `make test` — nowe moduły (wait-queue, przejścia stanów, kwant) w `COV_PATTERNS`, ≥90%.
- `make check-arch` — nic x86 nie wycieka do MI.
- QEMU headless: boot → bash → pętla forków, potoki (`yes | head`), `sleep`, Ctrl+C/Z,
  `cat` dużego pliku, `/proc/stat` `ctxt` przed/po, log `-d int` bez `v=08/0d/0e`.
- Brak regresji: job control (fg/bg), PTY/nterm, sygnały — scenariusze z poprzednich faz.
