# Uprawnienia użytkowników w NanOS

NanOS implementuje klasyczny linuksowy/POSIX-owy model **uznaniowej kontroli dostępu (DAC)**
i naprawdę go egzekwuje: istnieją prawdziwe konta nie-root, obowiązkowe logowanie na konsoli,
`su`, `sudo`, semantyka setuid/setgid/sticky, pełna rodzina poświadczeń z zachowanym
saved-set-id, grupy dodatkowe oraz uwierzytelnianie hasłem przez `/etc/shadow`. Ten dokument
opisuje, jak ten model jest poprowadzony przez jądro, VFS, warstwę syscalli, libc-glue,
dyskową bazę kont oraz przeniesione narzędzia sesyjne.

Dwie idee leżą u podstaw wszystkiego i warto je rozdzielić na wstępie:

1. **Rekord poświadczeń (`struct Cred`)** — tożsamość per-proces (uid+gid: rzeczywisty /
   efektywny / zachowany / filesystemowy + grupy dodatkowe), odpowiednik linuksowego
   `task_struct->cred`. Żyje na `Process` i jest jedynym źródłem prawdy o tym, "kim jest ten
   proces".
2. **Warstwa polityki (VFS)** — każda operacja na ścieżce jest sprawdzana względem `Cred`
   wywołującego w `fs/Vfs.cpp`, dokładnie tak, jak Linux uruchamia `inode_permission()` w
   VFS. Sterowniki filesystemów pozostają czystym mechanizmem: udostępniają metadane inode i
   wykonują operację, ale nigdy nie decydują, *czy* jest ona dozwolona.

To odzwierciedla Linuksa: wrażliwe na bezpieczeństwo decyzje są skupione w jednym, łatwym do
audytu miejscu, sterowniki FS pozostają niezależne maszynowo i testowalne na hoście bez
poświadczeń (`make check-arch` czysty), a czysta logika decyzyjna jest wyczerpująco testowana
jednostkowo.

> Uwaga o zakresie: linuksowe *capabilities* (`CAP_*`) są osobnym podsystemem rozdrabniania
> przywilejów nałożonym na klasyczny DAC i są tu poza zakresem. Przy braku capabilities NanOS
> używa dokładnie tego testu, którego sam Linux używa w tym przypadku — `euid == 0` to root,
> root ma wszystkie przywileje. Cały model ugo/setuid/saved-set-id/shadow/login/su/sudo jest
> zaimplementowany w pełni.

---

## 1. `struct Cred` — rekord poświadczeń (`kernel/Cred.h`)

```c
namespace kernel {
static const int NGROUPS_MAX = 32;
struct Cred {
    unsigned ruid, euid, suid, fsuid;   // real / effective / saved-set / filesystem uid
    unsigned rgid, egid, sgid, fsgid;   // ... gids
    int      ngroups;                   // valid entries in groups[]
    unsigned groups[NGROUPS_MAX];       // supplementary groups
};
}
```

`Process` (`kernel/Process.h:92`) jest właścicielem pola `Cred cred;` — to kanoniczny magazyn,
odpowiednik linuksowego `task_struct->cred`, inicjalizowany na roota przez `ProcTable::alloc`.
Obiekt `Syscalls` per-proces trzyma `Cred*` wskazujący na `cred` swojego procesu (w testach
hostowych test jest właścicielem `Cred` i wstrzykuje wskaźnik, więc logika poświadczeń jest
testowalna bez żywego jądra); `Syscalls::setCred()` przekierowuje go.

- **fork** kopiuje rodzicowy `Cred` do `Process::cred` dziecka i przekierowuje `Syscalls`
  dziecka na nie (`kernel/Exec.cpp:254`).
- **execve** zachowuje go, z wyjątkiem bitów setuid/setgid pliku (§4).

`Cred` to czyste dane bez metod — cała polityka żyje w czystych funkcjach poniżej, co czyni go
niezależnym maszynowo i testowanym na hoście.

---

## 2. Czyste funkcje decyzyjne (`kernel/Cred.cpp`)

Każda reguła dostępu/własności/przejścia jest funkcją totalną, wolną od efektów ubocznych,
działającą na czystych strukturach, więc każda gałąź jest testowana jednostkowo bez QEMU
(`tests/test_cred.cpp`). Wartości errno są strażowane (guarded), aby plik kompilował się
samodzielnie dla testu hostowego.

### Dostęp

`int credAccess(const Cred&, fileUid, fileGid, mode, want, useReal)` → `0` lub `-EACCES`
(`kernel/Cred.cpp:38`). `want` to maska bitowa `R(4) | W(2) | X(1)`.

- Wybiera triadę rwx **owner / group / other**, porównując właściciela pliku z **fsuid**
  (zwykłe otwarcia) albo **ruid** (`access(2)`, gdy ustawiony `useReal`), a grupę przez
  `egid`/`fsgid` (lub `rgid` dla `useReal`) **oraz** grupy dodatkowe `groups[]`.
- **Nadpisanie dla roota** podąża za *tym samym* zestawem id co sprawdzenie: program
  setuid-root wołający `access(2)` jest oceniany po swoim rzeczywistym uid, podczas gdy zwykłe
  otwarcie używa fsuid. Root dostaje r/w bezwarunkowo; **x jest przyznawane tylko, gdy
  ustawiony jest co najmniej jeden bit wykonania** gdziekolwiek w mode — wiernie wobec roota
  w Linuksie.

### Własność, sticky, metadane

| Funkcja | Reguła | Zwraca |
|---|---|---|
| `credInGroup(c, gid)` | `egid`/`fsgid` + skan grup dodatkowych | bool |
| `credMaySticky(c, dirUid, fileUid)` | usuwanie/zmiana nazwy w katalogu sticky: dozwolone iff `euid==0`, `fsuid==fileUid` lub `fsuid==dirUid` | `0` / `-EPERM` |
| `credMayChmod(c, fileUid)` | właściciel (`fsuid==fileUid`) lub root | `0` / `-EPERM` |
| `credMayChown(c, fileUid, newUid, newGid)` | zmiana właściciela → tylko root; zmiana grupy → root albo (własność pliku **i** członkostwo w `newGid`) | `0` / `-EPERM` |
| `credMayUtimes(c, fileUid, toNow)` | właściciel/root zawsze; `toNow` (`utimes(NULL)`) też dozwolone (VFS dodatkowo sprawdza zapis) | `0` / `-EPERM` |

### Przejścia (rodzina set\*id)

Każda mutuje `Cred` w miejscu i zwraca `0`, albo zwraca `-EPERM` i pozostawia go bez zmian.
Argument `-1` oznacza "pozostaw ten id bez zmian" (konwencja POSIX `setreuid`/`setresuid`).
Ścieżka uprzywilejowana to `euid == 0` (może ustawić cokolwiek); ścieżka nieuprzywilejowana
może ustawić każdy cel tylko na jedną z jego bieżących wartości rzeczywistej/efektywnej/
zachowanej. `fsuid`/`fsgid` podążają za `euid`/`egid`, z wyjątkiem `setfsuid`/`setfsgid`.

`credSetuid`, `credSeteuid`, `credSetreuid`, `credSetresuid` (+ odpowiedniki `…gid`),
`credSetfsuid`/`credSetfsgid` (każda zwraca *poprzedni* fsuid/fsgid, semantyka Linuksa) oraz
`credSetgroups` (tylko root; inaczej `-EPERM`, a `-EINVAL` gdy `n` poza zakresem).

---

## 3. Egzekwowanie DAC w VFS (`fs/Vfs.cpp`)

`Vfs` zyskuje prywatną warstwę polityki używaną przez każdą publiczną metodę; wirtualne metody
sterowników FS pozostają nietknięte. Poświadczenia wywołującego pochodzą z hooka
**`CredProvider`** — `typedef const Cred* (*CredProviderFn)()` (`fs/Vfs.h:124`). **Null jako
provider omija wszystkie sprawdzenia** (kontekst jądra / wczesny boot przed PID 1 =
równoważne rootowi), co odpowiada zachowaniu sprzed wprowadzenia uprawnień. Jądro instaluje
prawdziwy provider przy starcie (`kernel/SyscallDispatch.cpp:1096`), zwracający
`ProcTable::current()->cred` (`currentCred()` w `:1084`).

### Przejście po ścieżce (namei)

`Vfs::maySearch(path)` (`fs/Vfs.cpp:134`) statuje każdy **katalog-przodek** ścieżki `path` i
wymaga przeszukiwania (`x`) na każdym z nich. Działa przed każdą operacją, więc brak `x` na
dowolnym katalogu pośrednim daje `-EACCES`, dokładnie jak w Linuksie. Sprawdzenie
per-operacja następnie statuje obiekt docelowy i stosuje decyzję z Komponentu 2.

### Jak przebiega sprawdzenie uprawnień

```
syscall, e.g. open("/users/jan/notes", O_WRONLY)
   │
   ▼
Syscalls (kernel/Syscall.cpp)         resolve path vs cwd, then call the VFS
   │
   ▼
Vfs::write / read / create / ...      THE POLICY LAYER (inode_permission analogue)
   │   caller() ── CredProvider ──▶ ProcTable::current()->cred     (null ⇒ kernel/root: skip)
   │
   ├─ maySearch(path)      x on EVERY ancestor dir ─┐
   │                                                 │  each step stats the node:
   ├─ permission/mayCreate/mayDelete on the object ──┤    statNoCheck() → uid/gid/mode
   │                                                 ▼
   │                       credAccess(cred, uid, gid, mode, want, useReal)
   │                       credMaySticky / credMayChmod / credMayChown   (kernel/Cred.cpp)
   │                                                 │   pure, host-tested
   │                                                 ▼
   │                                       0   or   -EACCES / -EPERM
   ▼
FS driver (ExtFilesystem / RamFs / SynthFs)   pure MECHANISM: perform the op, no policy
```

### Sprawdzenia per-operacja

| Operacja | Sprawdzenia (`fs/Vfs.cpp`) |
|---|---|
| `read` / `readdir` | przeszukanie przodków; **R** na pliku/katalogu |
| `write` / `truncate` | przeszukanie przodków; **W** na pliku |
| `create` (nowy) | przeszukanie + **W** na rodzicu; nowy plik dostaje tożsamość wywołującego (patrz niżej) |
| `create` (istniejący, obcięcie) | **W** na pliku |
| `mkdir` / `mknod` / `symlink` | przeszukanie + **W** na rodzicu; własność + dziedziczenie setgid-dir |
| `unlink` / `rmdir` | przeszukanie + **W** na rodzicu; **sticky**: `credMaySticky` gdy rodzic ma `S_ISVTX` |
| `rename` | sprawdzenia usuwania na starym rodzicu + sprawdzenia tworzenia na nowym rodzicu |
| `link` | przeszukanie źródła + sprawdzenia tworzenia nowej nazwy |
| `chmod` | `credMayChmod`; ustawiającemu nie-root spoza grupy pliku zdejmowany jest `S_ISGID` |
| `chown` / `lchown` | `credMayChown`; udane chown przez nie-roota czyści bity setuid/setgid |
| `utimes` | `credMayUtimes`, z odwołaniem do reguły uprawnienia zapisu dla przypadku "teraz" |
| `stat` / `lstat` / `readlink` | tylko przeszukanie przodków (bez uprawnienia na obiekcie) |
| `access` (przez `Syscalls::permCheck`) | `credAccess(..., useReal=true)` — używa **rzeczywistych** id |

Wierność errno: `-EACCES` dla odmowy odczytu/zapisu/przeszukania/wykonania; `-EPERM` dla
operacji wymagających własności (chmod/chown przez nie-właściciela), naruszeń sticky i przejść
tylko-dla-uprzywilejowanych.

**Tożsamość nowego obiektu** (`Vfs::ownNewObject`, `fs/Vfs.cpp:303`): świeżo utworzony plik/
katalog jest stemplowany `fsuid` wywołującego; jego grupa to grupa katalogu-rodzica, gdy
rodzic jest setgid, w przeciwnym razie `fsgid` wywołującego; rodzic setgid propaguje też swój
bit `S_ISGID` na nowy podkatalog (semantyka BSD/Linux). Tu też naprawiono obsługę umask w
`mkdir`/`open`.

---

## 4. setuid/setgid przy `execve` (`kernel/Exec.cpp`) — sworzeń całości

`execve` egzekwuje **X** na pliku (`vfs->checkExec()`, `kernel/Exec.cpp:145`) i, po udanym
załadowaniu nowego obrazu (za punktem bez powrotu), stosuje bity setuid/setgid pliku przez
`credOnExec(p->cred, st.uid, st.gid, st.mode)` (`:215`).

`credOnExec` (`kernel/Cred.cpp:75`):
1. Jeśli `S_ISUID`: `euid = fsuid = file.uid`.
2. Jeśli `S_ISGID`: `egid = fsgid = file.gid`.
3. Zawsze: `suid = euid`, `sgid = egid` (Linux resetuje zachowane zestawy do nowych
   efektywnych id przy exec). `ruid`/`rgid` i grupy dodatkowe pozostają bez zmian.

To napędza przejścia przywilejów: `su`, `sudo`, `passwd`, `chsh` są instalowane jako
**setuid-root**, więc nieuprzywilejowany wywołujący zyskuje roota, by odczytać `/etc/shadow` i
zmienić tożsamość; `login` już działa jako root (dziecko PID 1) i *zrzuca* przywilej po
uwierzytelnieniu. Dyskowe bity setuid są honorowane jak są i są czyszczone przy chmod/chown
przez ustawiającego nie-root (§3), więc zmodyfikowany binarny setuid traci swój bit, jak w
Linuksie.

**`/proc/<pid>/status`** niesie wierne linie `Uid:`, `Gid:`, `Groups:`: `ProcInfo`
(`kernel/Process.h:165`) trzyma migawkę poświadczeń, a `SynthFs` renderuje wszystkie cztery
id w linii (`fs/SynthFs.cpp:193`). To zasila `id`, `ps` i podobne narzędzia.

---

## 5. Syscalle poświadczeń + opakowania libc

Syscalle poświadczeń żyją w testowalnym na hoście rdzeniu `Syscalls` (`kernel/Syscall.cpp:771`)
i delegują każdą decyzję do funkcji z §2:

`getuid`/`geteuid`/`getgid`/`getegid` (nigdy nie zawodzą), `setuid`/`setgid`,
`seteuid`/`setegid`, `setreuid`/`setregid`, `setresuid`/`setresgid`, `getresuid`/`getresgid`
(3 argumenty wyjściowe), `setfsuid`/`setfsgid` (zwracają poprzedni fs id), `getgroups` (gdzie
`size==0` zwraca liczbę) oraz `setgroups` (tylko root).

Są wpięte w obie ABI w `kernel/SyscallNr.h` z kanonicznymi numerami Linuksa — zestaw x86_64
(`SYS_setreuid 113 … SYS_setfsgid 123`, plus `getgroups 115`/`setgroups 116`) i zestaw i386
(`setreuid 70 … setfsgid 139 … setresuid 164 …`, plus warianty `*32` używane przez wywołujących
z 16-bitowym uid) — i routowane w `kernel/SyscallDispatch.cpp:737-783`.

### Opakowania libc i sentinel `(uid_t)-1` (`user/libc-glue/syscalls.c`)

Opakowania wykonują prawdziwe syscalle, więc userland widzi i zmienia per-procesowy `Cred`
jądra. Subtelna część to **sentinel "pozostaw bez zmian"**: POSIX zapisuje go jako `(uid_t)-1`,
ale `uid_t`/`gid_t` w picolibc są **16-bitowe**, więc `(uid_t)-1 == 65535` — a jądro
rozpoznaje tylko `-1`. Zwykłe rzutowanie podałoby jądru `65535` (rzeczywisty id) i *zmieniłoby*
id na niego. Pomocnik `idarg()` (`user/libc-glue/syscalls.c:105`) mapuje 16-bitowy sentinel z
powrotem na `-1`:

```c
static int idarg(uid_t v) { return v == (uid_t) -1 ? -1 : (int) v; }
```

dzięki czemu `setreuid`/`setresuid`/`setfsuid` (+gid) honorują sentinel. To była prawdziwa
przyczyna komunikatu sudo "unable to change to root gid": `setresgid((gid_t)-1, …)`
przełączał id na 65535. `seteuid`/`setegid` routują przez `setresuid`/`setresgid` (konwencja
glibc).

---

## 6. Baza kont

### Pliki kanoniczne (trwałe, na dysku ext pod `/disks/main/nanos/config/`)

| Plik | Format | Tryb |
|---|---|---|
| `passwd` | `name:x:uid:gid:gecos:home:shell` (pole 2 zawsze `x`; hash w shadow) | 0644 |
| `shadow` | `name:$6$salt$hash:lastchg:min:max:warn:inact:expire:flag` | 0600 root |
| `group` | `name:x:gid:member,member,…` | 0644 |
| `sudoers` | `root ALL=(ALL) ALL` + reguła `%wheel` | 0440 root |

Konta zalążkowe to `root(0)` i `jan(1000)`, grupa podstawowa `jan(1000)`, przy czym `jan` jest
członkiem `wheel(10)`; katalog domowy `jan` to `/disks/main/users/jan` (NanOS trzyma katalogi
domowe użytkowników pod **`/users`**, w układzie macOS/Plan 9, nie linuksowym `/home`).
Budowa obrazu zalążkuje cztery pliki bazy, tworzy `/users/jan` (1000:1000) i zalążkuje
`~/.bashrc` z `config/skel` (odpowiednik `/etc/skel`).

### Widoczność `/etc` (tmpfs a trwałość)

`/etc` to zapisywalny **RamFs tmpfs**, który jest *wypełniany przy starcie* z
`/disks/main/nanos/config/` przez `populateEtc()` (`kernel/Kernel.cpp:158`). Kopiuje on
`passwd`, `group`, `shadow`, `sudoers` (plus szablony sieciowe/logowania z `config/etc/`) do
tmpfs i następnie **wymusza zamierzony tryb i własność roota** — `shadow` na 0600, a `sudoers`
na 0440 — tak że nieuprzywilejowany użytkownik nie może odczytać hashy ani polityki sudo, mimo
że `create()` przepuścił tryb przez umask. Ponieważ `/etc` to tmpfs, edycje w runtime tam *nie*
są trwałe: narzędzie, które musi zmienić konto, zapisuje kopię dyskową pod `/nanos/config`
(patrz `chsh`, §7). Pełny obraz `/etc` znajdziesz w [filesystem.md](filesystem.md).

### `crypt($6$)` (`user/libc-glue/crypt.c`)

Samodzielna, clean-room implementacja SHA-512 `$6$`: rdzeń SHA-512 (FIPS 180-4) plus
opublikowane rozciąganie klucza sha512-crypt. Wynik zgadza się z `openssl passwd -6` / glibc
co do bajta (zweryfikowane wobec opublikowanych wektorów testowych w `tests/test_crypt.cpp`),
więc hash wygenerowany przez którekolwiek z nich weryfikuje się tutaj. Zaimplementowano tylko
`$6$` (domyślne w Linuksie); każdy inny prefiks zwraca `NULL`, zamiast ryzykować błędne
dopasowanie. Eksportowane przez `libc.ndl`.

### Dostęp do `/etc/group` i `/etc/shadow` (`user/libc-glue/grp_shadow.c`)

Clean-room, wzorowane na kontraktach POSIX: `getgrnam`/`getgrgid`/`getgrent` parsują
`/etc/group` *z listami członków*; `getspnam`/`getspent` parsują `/etc/shadow` (NanOS dodaje
`<shadow.h>` do sysroota SDK, którego picolibc nie ma); `getgrouplist`/`initgroups` liczą grupy
użytkownika jako jego gid podstawowy plus każda grupa w `/etc/group`, która go wymienia.
`user/libc-glue/pwd_grp.c` czyta `/etc/passwd` (tolerując `x` w polu 2) i dostarcza wariantów
reentrant `getpw*_r`, z wbudowanym fallbackiem na roota, gdy plik jest nieczytelny.

---

## 7. Wybór powłoki (na sposób Linuksa) i `chsh`

Powłoka logowania jest **per-użytkownik**, przechowywana w polu 7 `/etc/passwd` (`pw_shell`),
dokładnie jak w Linuksie. `login`, `su`, `init` i uruchamiacze terminala czytają ją przez
`getpwnam()` / `getpwuid()->pw_shell` i wykonują. Zalążek ma powłokę `root` jako `nsh`, a
`jan` jako **bash**.

Powłoką `jan` jest **rzeczywista ścieżka pakietu** `/disks/main/apps/bash/bash.nxe`, a *nie*
symlink z farmy linków `/disks/main/bin/bash.nxe` — ponieważ `execve` nie podąża za symlinkiem
ostatniego komponentu, więc PID 1 musi nazwać prawdziwy binarny plik.

`/etc/shells` (`config/etc/shells`) to rejestr poprawnych powłok logowania; `chsh`
(`user/chsh.c`) ogranicza wybór użytkownika do powłoki tam wymienionej, a użytkownik nie-root
może zmienić tylko *własną* powłokę. `chsh` jest instalowany jako setuid-root (tryb 0104755)
i przy zmianie zapisuje **zarówno** dyskową bazę (`/disks/main/nanos/config/passwd`, by zmiana
przetrwała restart), jak i żywą kopię tmpfs `/etc/passwd` (by `getpwnam()` zobaczył ją
natychmiast w tej sesji). `/etc/profile` (źródłowany przez powłoki logowania bash) ustawia
`PATH`, kolorowy `PS1` w stylu Ubuntu i ponownie eksportuje `VIMRUNTIME`/`VIMINIT` (przepływ
logowania resetuje środowisko).

---

## 8. Narzędzia sesyjne (porty) i przepływ logowania

- **toybox** (multicall 0BSD, tylko x86_64) dostarcza `login`, `su`, `passwd`, `id`, `groups`,
  `whoami`. Jest instalowany jako **jeden binarny plik setuid-root** `toybox.nxe` (tryb
  0104755) z **farmą twardych linków** per-komenda — `debugfs ln` tworzy `login.nxe`/`su.nxe`/…
  jako twarde linki do niego (`Makefile` `_image64`). `CONFIG_TOYBOX_SUID` w toybox zrzuca
  przywilej dla apletów nie-suid (`id`/`groups`/`whoami`), podczas gdy `login`/`su`/`passwd`
  zachowują roota, by odczytać `/etc/shadow` i zmienić tożsamość.
- **sudo** (Todd Miller, licencja ISC, tylko x86_64) to prawdziwe sudo z polityką sudoers
  dolinkowaną statycznie (bez PAM). Instalowane jako setuid-root (tryb 0104755); zalążkowane
  `/etc/sudoers` autoryzuje `%wheel`. Konfiguracja specyficzna dla NanOS w `config/sudoers`:
  `Defaults !use_pty` (NanOS nie może alokować pty komendy sudo), `secure_path` wskazujący na
  katalogi bin NanOS oraz `env_keep += "VIMRUNTIME VIMINIT"` (by `sudo vim` znalazł swój
  runtime przez `env_reset` sudo). `%wheel` jest obecnie `NOPASSWD` — interaktywny monit o
  hasło sudo przez `/dev/tty` jest sprawą otwartą, więc autoryzacja na razie odbywa się tylko
  przez członkostwo w grupie (ścieżka hasła jest weryfikowana osobno w terminalu GUI).
- **`init` (`user/init.c`)** — PID 1 (root) nie wykonuje już powłoki bezpośrednio. Otwiera tty
  konsoli i wykonuje toybox **`login`**, który pyta `login:` + `Password:`, weryfikuje wobec
  shadow przez `crypt()`, następnie `setgid(pw_gid)` → `initgroups(name, pw_gid)` →
  `setuid(pw_uid)` → `chdir(home)` → ustawia `HOME`/`SHELL`/`USER`/`PATH` → wykonuje powłokę
  logowania z `pw_shell`. Po wyjściu z powłoki `init` ponownie uruchamia `login` (w stylu
  getty). Terminal PTY/nterm dziedziczy już uwierzytelnione poświadczenia sesji, zamiast
  uwierzytelniać ponownie, co jest wierne wobec zachowania terminala graficznego.

---

## 9. Fallback wykonania `.nxe` i poprawka `+x` dla pakietów aplikacji

Dwa drobne, ale nośne szczegóły sprawiają, że narzędzia rozwiązują się i działają pod DAC.

**Fallback z sufiksem `.nxe`.** `Syscalls::nxeAppend()` (`kernel/Syscall.cpp:423`) dokleja
`.nxe` do ścieżki, której on brakuje. `stat`, `access` i `execve` ponawiają próbę z
`path + ".nxe"`, gdy nazwa goła jest nieobecna (`kernel/Syscall.cpp:435`, `:641`;
`kernel/Exec.cpp:135`), więc `sudo vim` / gołe `vim` rozwiązują `vim.nxe`. To **nigdy** nie
dotyczy tworzenia plików — tylko wyszukiwania/wykonania istniejącego programu.

**Binarne pliki pakietów wymagają jawnego `+x`.** Binarne pliki pakietów
(`/apps/<name>/<name>.nxe` dla vim, doom, netsurf, bash, …) są zapisywane do obrazu przez
`debugfs write`, co daje tryb 0644. Gdy VFS DAC egzekwuje teraz bit wykonania, `checkExec`
podąża za symlinkiem `/bin/<name>.nxe` do prawdziwego binarnego pliku i odmawia wykonania
(`-EACCES`) → "vim: Permission denied", a `access(X_OK)` sudo po `secure_path` go pomija →
"sudo: vim: command not found". Poprawka ustawia `mode 0100755` na każdym binarnym pliku
pakietu (`debugfs set_inode_field … mode 0100755`) w obu celach obrazu, obok istniejącego
`+x` na binarnych plikach systemowych i `init.nxe`.

---

## 10. Testowanie i weryfikacja

Testy hostowe (doctest, kontener `nanos-test`, bramka pokrycia linii ≥90%; `Cred` jest MI, więc
linkuje się do jądra *i* testów, a `make check-arch` pozostaje czysty):

- `tests/test_cred.cpp` — wyczerpująco po `credAccess` (każde ugo × want × root,
  real-vs-fs), `credInGroup`, `credMaySticky`, `credMayChmod`, `credMayChown`,
  `credMayUtimes` oraz każda gałąź przejść set\*id (uprzywilejowane + nieuprzywilejowane) +
  `setgroups`.
- `tests/test_vfs_perm.cpp` — egzekwowanie VFS wobec fikstur ext/RamFs ze zaślepionym
  `CredProvider`: zezwolenie+odmowa odczytu/zapisu/wykonania, przeszukanie ścieżki odmówione
  na przodku bez `x`, zapis-na-rodzicu + sticky dla create/unlink/rename, własność chmod/chown
  (`-EPERM` vs `-EACCES`), `access()` używające rzeczywistych id, umask przy create/mkdir oraz
  przejścia exec z bitami setuid/setgid.
- `tests/test_crypt.cpp` — opublikowane wektory testowe `$6$` muszą zgadzać się z `crypt()`.

Akceptacja w QEMU: boot → `login:` → zalogowanie jako `jan` → `id` pokazuje
`uid=1000(jan) gid=1000(jan) groups=1000(jan),10(wheel)` → `cat /etc/shadow` daje **Permission
denied** → `sudo id` działa jako root → `su -` osiąga monit roota → jako `jan` odmowa usunięcia
pliku należącego do roota z katalogu sticky i odmowa zapisu do `/nanos/bin`.

---

## Zobacz też

- [filesystem.md](filesystem.md) — przestrzeń nazw VFS, układ na dysku, tmpfs `/etc`,
  symlinki/twarde linki (farma linków `/bin` i farma twardych linków toybox).
- [syscalls.md](syscalls.md) — ABI syscalli, rdzeń `Syscalls`, dispatch i errno.
- [nxe-ndl.md](nxe-ndl.md) — format `.nxe` i loader, cykl życia procesu ring-3 oraz
  `libc.ndl` (eksportujący `crypt` i opakowania poświadczeń).
</content>
