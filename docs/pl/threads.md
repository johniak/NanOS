# Wątki NanOS (pthreads)

NanOS uruchamia prawdziwe wątki POSIX: dołączoną implementację musl pthread na bazie picolibc,
dostarczoną wewnątrz `libc.ndl`, nad modelem 1:1 wątków jądra. Program linkujący `-lpthread` na
Linuksie działa tu bez zmian. Programy weryfikujące to `user/pthrstress.c` (deterministyczny, ciężki
test obciążeniowy) oraz `user/pfract.c` (równoległy renderer Mandelbrota sterowany prawdziwą
kolejką roboczą puli wątków z mutexem i condvar).

## Model: wątki jądra 1:1

Każdy `pthread` to para **Task + Thread** jądra. Wszystkie wątki procesu **współdzielą**:

- **przestrzeń adresową** procesu (`AddressSpace` / page tables) — jeden heap, jeden zestaw mapowań,
- **tablicę deskryptorów plików**,
- procesowe **dyspozycje sygnałów** (obsługi `sigaction`).

Każdy wątek ma **własne**:

- kernel stack oraz stan rejestrów/`TrapFrame`,
- stos użytkownika (alokowany przez pthread_create z współdzielonego heapu),
- TID (`gettid`), blok TCB / TLS oraz `errno` (`__errno_location` zwraca slot per-wątek),
- **maskę** sygnałów (zbiór zablokowanych jest per-wątek; dyspozycje są per-proces).

Wątki są tworzone przez `clone(2)` (CLONE_VM|CLONE_FS|CLONE_FILES|CLONE_THREAD|CLONE_SETTLS|…),
kończą działanie przez `exit(2)`, a cały proces kończy się przez `exit_group(2)`. `pthread_join`
blokuje się na futexie TID dziecka (jądro zapisuje słowo clear_child_tid i budzi joinujących przy
wyjściu).

## Jednoprocesorowy: współbieżność, nie równoległość

NanOS działa na jednym rdzeniu. Wątki **przeplatają się** na jednym CPU pod kontrolą schedulera; nie
działają równocześnie na wielu rdzeniach. Równoległy Mandelbrot w `pfract.c` dzieli zatem *pracę*
pomiędzy wątki robocze, ale oblicza ją na jednym rdzeniu — wynik mierzony czasem ściennym jest taki
sam jak przy renderze szeregowym. Właśnie dlatego suma kontrolna `pfract` (złożenie liczby iteracji
ucieczki każdej komórki) jest deterministycznym dowodem poprawności: kolejka robocza decyduje jedynie,
*kto* oblicza każdy wiersz, nigdy *jaka* jest odpowiedź.

## TLS: jeden przeładowany deskryptor GDT

Thread-local storage używa konwencji segmentu GS na i386. `set_thread_area`/`CLONE_SETTLS`
instalują bazę TLS wątku w **jednym deskryptorze GDT**; przy każdym context switch scheduler
przeładowuje ten deskryptor (i `%gs`) dla wchodzącego wątku, więc `%gs:0` zawsze wskazuje na TCB
bieżącego wątku. `__pthread_self()` z musl oraz `errno` per-wątek opierają się na tym mechanizmie.

## Synchronizacja: oparta na futex

Wszystkie prymitywy blokujące są zbudowane na syscallu `futex(2)` (hostowo testowalny `FutexTable` nad
wait/wake schedulera): `pthread_mutex`, `pthread_cond`, `pthread_rwlock`, `pthread_barrier`,
`sem_t` oraz `pthread_once`. Szybkie ścieżki bez rywalizacji zostają w przestrzeni użytkownika
(atomowy CAS); tylko rywalizacja wchodzi do jądra. `pthread_spin_*` to czysty spin userlandu (bez
futexu).

## Obsługiwane API

- **Cykl życia:** `pthread_create`, `pthread_join`, `pthread_detach`, `pthread_exit`,
  `pthread_self`, `pthread_equal`, atrybuty (`pthread_attr_*`, w tym rozmiar stosu).
- **Mutexy:** `pthread_mutex_*` (normalny / rekurencyjny / errorcheck), `PTHREAD_MUTEX_INITIALIZER`.
- **Zmienne warunkowe:** `pthread_cond_*` (signal/broadcast/wait/timedwait).
- **Blokady odczytu-zapisu:** `pthread_rwlock_*`.
- **Bariery:** `pthread_barrier_*`. **Spinlocki:** `pthread_spin_*`.
- **Jednorazowa inicjalizacja:** `pthread_once`. **Klucze TLS:** `pthread_key_create/delete`,
  `pthread_setspecific/getspecific` (z destruktorami uruchamianymi przy zakończeniu wątku).
- **Semafory:** `sem_init/destroy/post/wait/trywait/timedwait/getvalue` (`<semaphore.h>`).
- **Anulowanie:** `pthread_cancel`, `pthread_setcancelstate/type`, `pthread_testcancel`,
  `pthread_cleanup_push/pop`.

## Sygnały

Sygnały są 64-bitowe (ABI `rt_sigaction`/`rt_sigprocmask`/`rt_sigpending`). Dostarczanie jest
per-wątek: `tgkill(2)` celuje w konkretny TID, a sygnał wysłany do procesu jest dostarczany do
wątku, który go nie blokuje. Anulowanie jest zaimplementowane na tej podstawie przez wewnętrzny
`SIGCANCEL`.

## Ograniczenia (zgodne z tym, co jest zbudowane)

- **Tylko anulowanie odroczone.** `PTHREAD_CANCEL_DEFERRED` działa z granularnością punktu
  anulowania. `PTHREAD_CANCEL_ASYNCHRONOUS` jest best-effort — jest honorowane przy następnym
  punkcie anulowania, a nie prawdziwie asynchronicznie, ponieważ nie ma mechanizmu
  `SA_SIGINFO`/`ucontext` do rozwijania stosu z dowolnej instrukcji.
- **Punkty anulowania to wyłącznie oczekiwania futex.** `pthread_cond_wait`/`timedwait` oraz
  oczekiwania na semaforach przechodzą przez `__syscall_cp` musl i *są* punktami anulowania.
  Blokujące wywołania picolibc (`read`, `write`, `sleep`, …) **nie** są kierowane przez
  `__syscall_cp` w tej hybrydowej libc, więc **nie** są punktami anulowania — wątek zablokowany
  w `read()` nie zostanie anulowany, dopóki nie powróci i nie trafi na jawne `pthread_testcancel()`.
- **Brak `pthread_atfork`.** `fork()` w procesie wielowątkowym daje dziecku tylko wątek wywołujący
  (POSIX), ale locki trzymane przez inne wątki w chwili fork pozostają zablokowane w dziecku.
  Trzymanie locka libc w poprzek `fork()` jest zatem niebezpieczne, chyba że dziecko **natychmiast
  wykonuje exec** (typowy wzorzec w stylu bash, który jest ćwiczony w pod-teście fork/exec
  `pthrtest`).
- **Brak `SA_SIGINFO`/`ucontext`** dla obsług sygnałów (brak ładunku siginfo, brak kontekstu
  maszynowego).
- **Jednoprocesorowy** — brak prawdziwego przyśpieszenia równoległego (patrz wyżej).
- **Stosy wątków odłączonych wyciekają do końca procesu.** `munmap` jest już prawdziwy
  (`SYS_munmap` zwalnia ramki *i* odzyskuje VA przez per-procesową listę wolnych mmap), więc
  `pthread_join` odzyskuje stos+TCB dołączonego wątku, a długotrwały churn tworzenia/łączenia
  recykluje ograniczone 64 MiB okno mmap. **Odłączony** wątek jednak nie może odmapować własnego
  działającego stosu i nikt go nie łączy, więc jego stos+TCB są odzyskiwane dopiero przy
  zakończeniu procesu. Przenośną poprawką jest `__unmapself` z musl (trampolina i386, która
  przełącza się na mały współdzielony stos, odmapowuje mapowanie martwego wątku, a następnie
  wykonuje `SYS_exit`); nie jest tu vendorowana. Programy, które **łączą** swoje wątki (typowy
  przypadek, który ćwiczy `pthrstress`), recyklują VA bez ograniczeń.
- **Per-procesowa lista wolnych mmap ma stały rozmiar** (`Process::NMMAPFREE`, 64 wpisy). Jeśli
  proces odmapuje więcej różnych, niekoalescujących zakresów niż to bez ponownego mmapowania,
  nadmiarowe zwolnienia nadal odzyskują RAM fizyczny, ale tracą VA (logowane raz). Wystarczające
  dla churnu wątków, gdzie każde join zwalnia region, który następne create natychmiast ponownie
  używa.

## Weryfikacja (QEMU)

- **Obciążenie (`pthrstress`):** 32 wątki robocze atakujące mutex-chroniony licznik do `640000`
  (dokładnie — bez utraconych aktualizacji), producent/konsument na condvar oraz **100 rund churnu
  tworzenia/łączenia 32 wątków** (3200 cykli życia wątków). Wynik: `pthrstress: 32 threads,
  counter=640000, churn 100 rounds ok`, 0 błędów CPU. Ten churn ujawnił pierwotny wyciek stosu
  mmap (naprawiony przez prawdziwy `munmap` + ponowne użycie VA, patrz wyżej).
- **Równoległy render (`pfract`):** `pfract: 8 workers, 1540 cells, checksum=0x… ok` z narysowanym
  zbiorem Mandelbrota w ASCII — prawdziwa kolejka robocza puli wątków z mutexem i condvar.
- **Zapas RAM kernela dla stosów:** `KHeapFree` w `/proc/meminfo` jest **identyczny przed i po**
  churnie 3200 wątków (np. `32350 kB` → `32350 kB`). Kernel stack 32 KiB każdego wątku jest
  alokowany na stercie przy tworzeniu i w całości odzyskiwany, gdy scheduler sprząta task DONE,
  więc kernel heap wraca do bajtu bez wycieku; szczytowe równoczesne zużycie stosu jądra
  (~32–64 żywych wątków ≈ 1–2 MiB) mieści się z dużym zapasem w heapie. Sufit slotów tasków
  (`MAXTASKS = ProcTable::MAX + 8`) jest wystarczający i celowo **nie** został obniżony.
