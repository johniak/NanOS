# Scheduler i model procesów NanOS

Wywłaszczający, jednoprocesorowy scheduler round-robin zbudowany na **deferred preemption**: timer IRQ
nigdy sam nie przełącza tasków — jedynie sygnalizuje, że należny jest reschedule, a właściwy context switch
następuje w dobrze zdefiniowanym bezpiecznym punkcie (powrót do ring 3 lub dobrowolny yield w jądrze).
To model `ret_from_intr` z Linuksa i to serce projektu: jądro nigdy nie jest
przełączane na dowolnej instrukcji ring 0, więc ramki przerwań i syscalli nigdy nie przeplatają się
na kernel stacku taska.

Kod oddziela politykę MI od mechanizmu MD: `kernel/Scheduler.{h,cpp}` jest właścicielem tablicy tasków,
stanów, wyboru round-robin oraz logiki block/wake; kontrakt `<arch/sched.h>` dostarcza
CPU context switch, sfabrykowany stos pierwszego uruchomienia oraz timer wywłaszczania (`arch/x86/cpu/
switch.S` + `sched_x86.cpp`). Warstwa procesów (`kernel/Process.*`, `kernel/Exec.cpp`) leży na
wierzchu. Jak programy wchodzą w ring 3 i jak budowane są ramki sygnałów — zobacz nxe-ndl.md §6; o
sieciowych wątkach jądra zobacz networking.md §3.

---

## 1. Task vs Process

Dwa odrębne obiekty:

- **`Task`** (`kernel/Scheduler.h`) — to, co uruchamia scheduler: kernel stack + funkcja body +
  zapisany kontekst. Z punktu widzenia schedulera *wszystko* jest wątkiem jądra; jeden task (init)
  akurat wchodzi w ring 3, co jest ortogonalne wobec schedulowania.
- **`Process`** (`kernel/Process.h`) — proces uniksowy: pid, przestrzeń adresowa, tablica fd, stan
  sygnałów, grupa job-control. Proces jest właścicielem jednego taska.

Wskazują na siebie nawzajem, tak że każde wyszukanie jest O(1): `Task::proc` (skierowanie syscalla do
jego procesu) oraz `Process::task` (znalezienie uruchamialnego taska dla pid). `ProcTable::setCurrent(task->proc)`
jest wywoływane przy każdym przełączeniu, by syscalle trafiały we właściwy proces bez skanu O(n).

```cpp
struct Task {
    unsigned kesp;        // saved kernel esp — the whole context lives on the stack
    unsigned esp0;        // top of this task's kernel stack (loaded into TSS.esp0 when it runs)
    TaskState state;
    void (*body)();
    int id;               // 0 = idle
    unsigned char* kstack;
    unsigned wakeAt;      // BLOCKED-with-deadline: tick at which onTick re-wakes it (0 = none)
    Process* proc;        // owning process back-pointer
    Task* waitNext;       // intrusive link while parked on a WaitQueue
};

enum TaskState { TASK_READY, TASK_RUNNING, TASK_BLOCKED, TASK_STOPPED, TASK_DONE, TASK_ZOMBIE, TASK_FREE };
```

| Stan | Znaczenie |
|---|---|
| `TASK_READY` / `TASK_RUNNING` | uruchamialny / aktualnie na CPU |
| `TASK_BLOCKED` | czekający na WaitQueue, terminie czasowym lub ponowieniu I/O |
| `TASK_STOPPED` | zawieszony przez job-control (SIGSTOP/SIGTSTP) — tylko `resume()` go odwiesza |
| `TASK_DONE` | body wątku jądra zwróciło sterowanie; leniwie sprzątane przez `onTick` |
| `TASK_ZOMBIE` | proces zakończył się, czeka na `waitpid` rodzica |
| `TASK_FREE` | slot ponownie użyteczny przez `findFreeSlot()` |

**Sloty** tasków to statyczna tablica `g_tasks[MAXTASKS]` (`MAXTASKS = ProcTable::MAX + 8 = 1032`);
kosztowna część — kernel stack **32 KiB** każdego taska (`KSTACK_SIZE`) — jest alokowana na stercie przy
tworzeniu i zwalniana przy sprzątaniu, więc liczba żywych tasków jest ograniczona przez RAM, a nie
tablicę. (Stos ma 32 KiB, ponieważ ścieżka ext-write + dziennik JBD2 zagnieżdża kilka 4 KiB buforów
bloków, a IRQ klawiatury może wylądować na wierzchu; kanarek na granicy sterty zamienia przepełnienie w
czysty panic.)

---

## 2. Deferred preemption — kluczowy mechanizm

### Tick timera *nie* przełącza

`Scheduler::onTick(fromUser)` (sterowany przez 1000 Hz PIT IRQ0) tylko:

1. `g_ticks++`;
2. księguje tick na konto bieżącego procesu (user vs system, wedle ring, który przerwał);
3. próbkuje load average co 5 s (EWMA z Linuksa, `loadDecay`);
4. budzi każdy task `TASK_BLOCKED`, którego termin `wakeAt` nadszedł, i leniwie sprząta wątki jądra
   `TASK_DONE`;
5. ustawia **`g_needResched = true`** — ale tylko na granicy kwantu lub gdy śpiący właśnie się obudził
   (`shouldResched`), tak by dwa taski CPU-bound nie wymieniały się CPU (i nie flushowały TLB) 1000×/s.

Kwant to `QUANTUM = 10` ticków (10 ms): task RUNNING trzyma CPU aż jego wycinek wygaśnie,
chyba że zakończenie I/O / wygasły timer uczyni uruchamialnym śpiącego o wyższym priorytecie, co go
wywłaszcza niezwłocznie.

### Przełączenie następuje tylko w bezpiecznych punktach

```
timer IRQ0 ──► onTick(): g_ticks++, wake sleepers, set g_needResched   (NO switch)
                                │
        ┌───────────────────────┴───────────────────────┐
        ▼                                                 ▼
ret-to-ring3 from an IRQ                       voluntary yield/block in kernel
(irq.S tests saved CS & 3)                     (schedule / block / sleepOn / ioWait)
   call schedPreempt() ─► preempt():                     │
     if g_needResched: schedule()                        ▼
        └──────────────────────────────► schedule(): pick next + archContextSwitch
```

- **Wywłaszczenie mimowolne** następuje *tylko* na ścieżce powrotu z przerwania sprzętowego **do ring
  3**. `irq.S` sprawdza zapisany CS (`test dword [esp+48], 3`): jeśli powraca do ring 0, pomija
  reschedule w całości — jądro nigdy nie jest wywłaszczane w środku instrukcji. Gdy powraca do ring 3,
  wywołuje `schedPreempt` → `Scheduler::preempt()`, które przełącza wtw `g_needResched`. W tym momencie
  kernel stack trzyma pełną, czystą trap frame, więc przełączenie jest bezpieczne.
- **Przełączenia dobrowolne** następują, gdy kod jądra wywoła `schedule()` bezpośrednio — przez
  `yield()`, `block()`, `sleepUntil()`, `sleepOn()`, `ioWait()`, albo gdy body taska zwróci sterowanie.

Samo `schedule()` wykonuje księgowanie w krótkiej sekcji `cpuIrqSave`/`cpuIrqRestore` (tak by równoległy
`onTick` nie wpłótł się w współdzielone stany `g_tasks`): wybiera następny uruchamialny task
(`pickNext`, round-robin, idle tylko gdy nic innego nie działa), przerzuca pola RUNNING/READY,
`setKernelStack(next->esp0)`, `setCurrent(next->proc)` — a następnie **przywraca IF przed** właściwym
`archContextSwitch`, który nie dotyka współdzielonego stanu i działa z oryginalnym IF wywołującego.

> Dlaczego odroczone? Context switch w środku IRQ albo uszkodziłby kernel stack przerwanego taska, albo
> wymagałby kopiowania częściowej ramki. Odroczenie do powrotu do ring 3 (lub dobrowolnego yield)
> utrzymuje kernel stack każdego taska jako czysty stos kompletnych ramek. **Nie wprowadzaj ponownie
> przełączania wewnątrz obsługi IRQ.**

---

## 3. Context switch (arch)

`archContextSwitch(unsigned* saveOldKesp, unsigned newKesp)` (`switch.S`): wkłada na stos rejestry
zachowywane przez wywoływanego (`ebx/esi/edi/ebp`) **oraz CR3**, zapisuje `esp` do `*saveOldKesp`,
ładuje `newKesp`, a następnie zdejmuje — przeładowując CR3 (i flushując TLB) **tylko jeśli faktycznie się
zmieniło**, tak że przełączenie wątek jądra ↔ wątek jądra (ta sama przestrzeń adresowa) pomija flush.
Cały kontekst żyje na kernel stacku; `kesp` to jedno słowo, które przechowuje `Task`.

Świeżo utworzony task nie ma realnej ramki, do której mógłby wrócić, więc `archTaskBootstrap`
**fabrykuje** jedną: umieszcza `(cr3, ebp=0, edi=0, esi=0, ebx=0, ret=taskTrampoline)`, tak by pierwsze
przełączenie "wróciło" do `taskTrampoline`, które wykonuje `sti` (wątki jądra działają z włączonymi
przerwaniami) i wywołuje `schedulerRunCurrentBody` → `Scheduler::runCurrentBody()` → body taska. Gdy
body zwraca sterowanie, task jest oznaczany `TASK_DONE` i leniwie sprzątany.

`fork` natomiast fabrykuje stos dziecka z **syscallowej trap frame rodzica**
(`archForkChild`), tak że pierwsze przełączenie w dziecko wykonuje `ret` przez `ret_from_fork` i `iret`
kopiowanej ramki z `eax = 0` do ring 3 — zobacz §5.

**Timer**: `archTimerInit(1000)` programuje kanał 0 PIT (dzielnik `1193180/1000 = 1193`) i
kieruje IRQ0 do `timerTick`, który wywołuje `onTick((cs & 3) == 3)`. `Scheduler::ticks()` (globalny
`g_ticks`) to zegar monotoniczny stojący za `nanosleep`, timeoutami poll/select, timerami TCP oraz
`/proc/uptime`.

---

## 4. Blokowanie i wybudzenia

Taski blokują się na **`WaitQueue`** (`kernel/WaitQueue.h`) — intruzywnej liście jednokierunkowej po
`Task::waitNext`, czystej strukturze danych, bez zależności od schedulera/arch. Prymitywy:

| Prymityw | Zastosowanie |
|---|---|
| `sleepOn(q)` | zaparkuj na kolejce zdarzeń aż do `wakeAll(q)` lub sygnału; odlinkuj przy powrocie. |
| `sleepOnUntil(q, ready, ctx)` | pętla, ponowne testowanie `ready(ctx)` przy wyłączonych przerwaniach — domyka okno utraconego wybudzenia dla wybudzacza **sterowanego IRQ** (klawiatura wypełniająca bufor konsoli). |
| `sleepUntil(tick)` | blokuj do terminu; `onTick` go budzi. Jedno wybudzenie na długi sen, nie jedno na tick. |
| `block()` | ogólny deschedule (stop job-control, ponowienie legacy). |
| `ioWait()` | `sleepUntil(now+1)` — ponowienie I/O legacy, ponowne sprawdzenie warunku w następnym ticku. |

Wybudzenia tylko **flagują** zmianę stanu, nigdy nie przełączają (bezpieczne wobec IRQ):

- `wake(t)` — `TASK_BLOCKED → TASK_READY` + `g_needResched`. Zabezpieczone tak, by dotykać tylko tasków
  BLOCKED, więc nigdy nie wskrzesi `TASK_ZOMBIE`/`TASK_DONE` w jego finalne `for(;;)`.
- `resume(t)` — `TASK_STOPPED → TASK_READY`, utrzymane jako odrębne od `wake()`, tak by zwykłe wybudzenie
  I/O lub SIGCHLD nie mogło odwiesić procesu wstrzymanego Ctrl+Z — robi to tylko SIGCONT/SIGKILL.
- `wakeAll(q)` — uczyń uruchamialnym każdy task zaparkowany na `q`; przechodzi listę pod `cpuIrqSave` z
  zabezpieczeniem ograniczonym do slotów, tak że uszkodzona kolejka degraduje się do no-op zamiast faultować.

Wyścig utraconego wybudzenia jest domknięty przez wykonanie *enqueue + przerzut na BLOCKED* pod
`cpuIrqSave`: jeśli wybudzacz IRQ wystrzeli w luce przed `schedule()`, zastaje task już READY, a
`schedule()` po prostu wybiera go ponownie. Każdy prymityw blokujący najpierw sprawdza też
`hasPendingSignalCurrent()`, tak że task nigdy nie przesypia sygnału już do niego wysłanego (podstawa
EINTR / restartu).

> To blokowanie sterowane zdarzeniami jest powodem, dla którego wątki jądra muszą **wykonywać
> yield/block na WaitQueue**, a nie spinować. Sieciowy softirq RX (`ksoftirqd-net`) śpi na kolejce, którą
> `netifRx` budzi z IRQ e1000 (networking.md §3). Jedynym uprawnionym spinem jest **task idle**
> (`idleBody`: `halt_or_hlt` = `sti; hlt`, potem `schedule()`), który działa w ring 0 i dlatego nigdy nie
> jest wywłaszczany — musi dobrowolnie ustąpić wszystkiemu, co `onTick` właśnie uczynił uruchamialnym.

---

## 5. Cykl życia procesu

`Process` (`kernel/Process.h`) niesie: `pid`, `parent`, `task`, `space` (`arch::AddressSpace*`),
`sys` (`Syscalls*` = tablica fd per-proces + status wyjścia), `exitCode`/`termSignal`, `kthread`,
`comm`/`cmdline`, stertę (`brkBase/brkCur/brkMax`) oraz wskaźnik bump `mmapNext`, identyfikatory
job-control `pgid`/`sid` oraz `SignalState`. `ProcTable` trzyma `g_procs[MAXPROC = 1024]`; pidy pochodzą z
monotonicznie rosnącego `g_nextPid` (**nigdy nie używane ponownie**); `byPid` to skan liniowy.

**Taski rozruchowe** (`kernel/Kernel.cpp`): `Scheduler::init()` tworzy task idle (slot 0);
`registerKthread` owija `Task` w `Process` oznaczony `kthread` (tak że pokazuje się w `/proc` jak
linuksowy `[kworker]`); init to **pid 1**, uruchamia `initTaskBody`, które wykonuje `execProgram` na
`/disks/main/nanos/core/init.nxe` do ring 3; rdzeń sieci rejestruje `ksoftirqd-net` i
`net-timer` jako kthreads. Następnie `archTimerInit(1000)` i `Scheduler::start()` przełączają w pierwszy
uruchamialny task i nigdy nie wracają.

- **`fork`** (`forkProcess`, `Exec.cpp`): zaalokuj pid dziecka, **zachłannie skopiuj** przestrzeń adresową
  (`mmuCopyAddressSpace` — obraz, sterta, okna modułów i mmap), zduplikuj tablicę fd, odziedzicz
  `pgid`/`sid` i dyspozycje sygnałów, `createBlank` task, a `archForkChild` fabrykuje jego
  kesp z trap frame rodzica. Rodzic dostaje pid dziecka; dziecko zwraca 0.
- **`execve`**: zastąp obraz w miejscu (ten sam pid, świeża przestrzeń adresowa), `archFrameToUser`
  przepisuje trap frame, tak że `iret` syscalla wchodzi w nowy program, resetuje przechwycone obsługi
  sygnałów, zamyka deskryptory `FD_CLOEXEC`.
- **`exit`** (`procExit`): oznacz task `TASK_ZOMBIE`, wyślij `SIGCHLD` do rodzica i obudź go,
  przepnij wszelkie dzieci do init, a następnie wykonaj `schedule()` w bok.
- **`waitpid`** (`waitProcess`): sprzątnij dziecko-zombie — `Scheduler::reap` zwalnia stos 32 KiB,
  `delete sys`, zwalnia slot proc — i zwróć zakodowany status; blokuje (przerywalnie sygnałem),
  jeśli żadne dziecko nie jest gotowe, a `WNOHANG` nie jest ustawione; `-ECHILD`, gdy nie ma żadnych.

**Job control:** każdy proces ma `pgid`/`sid`; `setpgid`/`setsid` tworzą grupy i sesje.
Terminal śledzi pierwszoplanową grupę procesów; Ctrl+C/Ctrl+Z na PTY dostarczają SIGINT/SIGTSTP do
całej tej grupy (`consoleSignalGroup`). `/proc/<pid>/…` jest generowane z tablicy proc.

---

## 6. Sygnały i schedulowanie

`SignalState` trzyma maski `pending`/`blocked`/`restart`, per-sygnałowe `handlers[]` oraz adres
trampoliny `restorer`. Sygnały są **wysyłane** asynchronicznie (`sigPost` ustawia bit pending),
ale **dostarczane na tej samej odroczonej granicy co wywłaszczenie** — powrocie do ring 3:
`signalDeliver(tf, origEax, inSyscall)` działa ze ścieżki powrotu syscalla (`syscall_x86.cpp`, po
syscallu, `inSyscall=true`) oraz ze ścieżki powrotu z przerwania sprzętowego (`Interrupt.cpp`,
`inSyscall=false`). Rozwiązuje każdy oczekujący, odblokowany sygnał (terminate / ignore / stop /
continue / run handler) i — dla handlera — wywołuje `archPushSignalFrame`, by zbudować ramkę sygnału na
stosie użytkownika (zobacz nxe-ndl.md §6.5).

Ponieważ dostarczenie jest odroczone do granicy ring 3 i nigdy nie zachodzi w kontekście IRQ, sygnał
wysłany z przerwania jest po prostu kolejkowany i dostarczany, gdy cel następnym razem wróci do trybu
użytkownika. Task zablokowany na WaitQueue jest czyniony uruchamialnym przez wysłanie (`wake`), a potem
zauważa oczekujący sygnał przy powrocie i albo restartuje syscall (`SA_RESTART`), albo zwraca `-EINTR`.

---

## 7. Niezmienniki i status

- **Jeden CPU.** Brak SMP, brak spinlocków, brak kolejek run per-CPU. Wzajemne wykluczanie wobec
  timera/IRQ to `cpuIrqSave`/`cpuIrqRestore` (push flags + `cli` / restore).
- **Stan przerwań.** Syscalle działają z IF=0 (`isr128` wykonuje `cli`); wątki jądra działają z IF=1
  (trampolina `sti`). `schedule()` przywraca IF wywołującego w poprzek przełączenia.
- **Punkty wywłaszczenia są dokładnie dwa:** powrót do ring 3 z IRQ oraz dobrowolne
  `schedule()`. Kod ring 0 (w tym wątki jądra pomiędzy yieldami) nigdy nie jest mimowolnie
  przełączany — wątki jądra muszą współpracować, blokując się na WaitQueue lub śpiąc.
- **Historia.** Obecny scheduler jest w pełni żywy (Stage 3+). Zaparkowany `arch/x86/cpu/
  MultiTasking.cpp` to dawna, wyłączona próba tylko-EIP — nieużywana. (Linia z CLAUDE.md "multitasking
  experimental, disabled" poprzedza ten scheduler.)

### Kluczowe pliki

| Komponent | Plik |
|---|---|
| scheduler MI (tablica, stany, polityka, block/wake) | `kernel/Scheduler.{h,cpp}` |
| kontrakt MI/MD | `arch/include/arch/sched.h` |
| context switch + trampolina | `arch/x86/cpu/switch.S` |
| bootstrap, timer, most preempt | `arch/x86/cpu/sched_x86.cpp` |
| brama ring-3 deferred-preemption | `arch/x86/cpu/irq.S` |
| listy oczekiwania | `kernel/WaitQueue.h` |
| tablica procesów | `kernel/Process.{h,cpp}` |
| fork / execve / exit / wait | `kernel/Exec.cpp` |
| sygnały + dostarczanie | `kernel/Signal.{h,cpp}`, `signalDeliver` (Exec.cpp), `syscall_x86.cpp` / `Interrupt.cpp` |
| okablowanie rozruchu (idle, init, kthreads) | `kernel/Kernel.cpp` |
