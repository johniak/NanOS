# 02 - Node.js And V8 On NanOS

**Goal:** Build a working `node.nxe` for NanOS x86_64, with V8 executing JavaScript, filesystem and
network basics working, and enough native-addon infrastructure for Electron app dependencies.

**Why first:** Electron embeds Node. If Node/V8 cannot run independently, Electron debugging becomes
unmanageable.

**Entry criteria:** plan 00 gate green (versions pinned) and plan 01 Tasks 1.2 + 1.3 green
(`epolltest` and `mmapexectest` PASS in QEMU — libuv needs epoll, V8 needs W^X `mprotect`).

## Decisions

- **Source: Electron's Node fork at the pinned version** (`node_version` from
  `manifest/electron-stack.lock`, checked out from `https://github.com/electron/node` or the DEPS
  URL recorded there). One V8/Node port effort serves both standalone `node.nxe` and Electron.
  If standalone build of the fork proves broken in a fork-specific way, fall back to upstream
  `nodejs/node` at the same version and record the delta in `docs/en/node-port.md` — but try the
  fork first.
- **Build identity: `--dest-os=linux` + `-D__NANOS__`.** Node/gyp has no pluggable OS list worth
  fighting; NanOS is close enough to linux/musl. Every NanOS-specific change is guarded by
  `#ifdef __NANOS__` and lives in `ports/node/patches/` — honest at code level without inventing
  a new dest-os. (Chromium takes the opposite, real-`target_os` route because Ozone forces it;
  see plan 03.)
- **Native addons: statically registered, no `.node` dlopen.** NanOS has no ELF `.so` loader —
  `dlopen()` is a permanent NULL stub, and NanOS's own dynamic format is `.ndl`
  (import-by-name). Required native modules are compiled into the binary and registered via
  Node's linked-module mechanism (`node::node_module` registration / `--link-module` configure
  flag). A future `.ndl`-backed `process.dlopen` is a documented follow-up, not part of this
  program.
- **First build shape: small.** `--without-intl --without-inspector --without-node-snapshot
  --without-npm --without-corepack`; V8 wasm trap handler off (no sigaltstack — plan 01
  Decisions). Re-enable intl (`small-icu`) only when MarkText/Electron demand it, recording size.
- **First execution: V8 jitless.** Run the first NanOS gates with `--jitless` (V8 interprets,
  allocates no executable memory) — this decouples "does Node run at all" from the W^X/mprotect
  path entirely. Enabling the JIT (default mode) is then its own gate, and any fault it
  introduces is by construction an executable-memory issue → `mmapexectest` territory. Two small
  gates instead of one huge one.
- **crypto: Node's bundled OpenSSL.** The NanOS system OpenSSL 3.0.15 port stays for system
  tools; sharing one library is a follow-up. Entropy comes from `getrandom` (exists).

## Files

**Create in NanOS repo:**
- `scripts/electron/build-node.sh`
- `scripts/electron/node-smoke.js`
- `scripts/smoke-node.sh`
- `docs/en/node-port.md`
- `ports/node/README.md`, `ports/node/patches/`
- `Makefile` target `node` (host wrapper following the `grep`/`toybox` SDK-container pattern)

**External work dir:**
- `$(SDK_WORK)/node-src/` (checkout), `$(SDK_WORK)/node-build/` (out-of-tree artifacts if used)

## Task 2.1: Fetch And Baseline

- [ ] Write `scripts/electron/fetch-node.sh`: clone the pinned Node (see Decisions) into
  `$SDK_WORK/node-src`, `git checkout <node_version from lock>`, write
  `$SDK_WORK/node-src/NANOS_REVISION.txt`, verify with `git rev-parse HEAD`.
- [ ] Sanity-build **for the host first** (unmodified tree):

```sh
cd "$SDK_WORK/node-src" && ./configure && make -j8   # host build, proves the checkout is sane
out/Release/node -e "console.log(process.versions)"
```

- [ ] Record versions printed (node, v8, uv, openssl) in `docs/en/node-port.md`.
- [ ] Gate: host node prints its versions. Commit the fetch script + doc.

## Task 2.2: Cross Configure For NanOS

- [ ] Write `scripts/electron/build-node.sh` that runs configure with the NanOS toolchain
  (mirroring how `scripts/nx-port-build.sh` recipes set CC/CXX/AR/LD for the SDK):

```sh
export CC=x86_64-nanos-gcc CXX=x86_64-nanos-g++ AR=x86_64-nanos-ar
export CC_host=cc CXX_host=c++              # host tools stay host
./configure --dest-os=linux --dest-cpu=x64 --cross-compiling \
  --without-intl --without-inspector --without-node-snapshot \
  --without-npm --without-corepack \
  --openssl-no-asm
make -j8
```

  plus `CFLAGS/CXXFLAGS += -D__NANOS__`. Exact flag set will evolve; the script is the record.
- [ ] Classify every failure with the plan-03 ladder (missing libc API | missing syscall |
  platform conditional | feature to disable | NanOS bug). Missing-API rows go to the gap matrix
  (plan 01 protocol) before implementing.
- [ ] Patches go to `ports/node/patches/` with the standard header (EXECUTION_GUIDE), applied by
  `build-node.sh` before configure.
- [ ] Gate: configure completes reproducibly; `make` reaches compiling V8 (`deps/v8/...` objects
  appear). Link failures are the next task's business.

## Task 2.3: V8 Bring-Up And First Execution

- [ ] Disable the wasm trap handler and any sigaltstack path via supported V8 GN/gyp defines
  (`V8_TRAP_HANDLER_SUPPORTED=0` shape — find the exact knob for the pinned version and record
  it in `docs/en/node-port.md`).
- [ ] Link `node.nxe`: run the produced ELF through the existing SDK `.nxe` conversion
  (`mknx` flow used by every port — see `scripts/nx-port-build.sh`).
- [ ] Add Makefile target `node` (host wrapper: SDK container build + `cp` into `bin/`), stage
  into the image, `make image64`.
- [ ] Gate inside NanOS (QEMU `run64`, login `jan`/`jan`) — **two stages** (see Decisions):

```sh
node.nxe --jitless -e "console.log(1 + 2)"   # stage 1: interpreter only, no exec pages
node.nxe -e "console.log(1 + 2)"             # stage 2: JIT on — exercises W^X for real
```

Expected output both times:

```text
3
```

Stage 1 green + stage 2 faulting = a JIT/executable-memory gap; go straight to `mmapexectest`.

If it faults instead: reproduce with the plan-01 microtests first (`mmapexectest`,
`epolltest`); a microtest that still passes while node faults means a new gap — add the matrix
row and a new microtest before touching V8.

## Task 2.4: Node Core APIs

- [ ] Write `scripts/electron/node-smoke.js` now (it is the acceptance oracle for this task):

```js
// node-smoke.js — NanOS Node acceptance: versions, fs, net, dns, timers, promises.
const fs = require('fs');
const net = require('net');
const dns = require('dns');

let failures = 0;
const ok = (cond, name) => {
  console.log((cond ? '  OK  : ' : '  FAIL: ') + name);
  if (!cond) failures++;
};

console.log('=== node-smoke ===');
console.log('node ' + process.versions.node + ' v8 ' + process.versions.v8);

// fs round-trip
const p = '/tmp/node-smoke.txt';
fs.writeFileSync(p, 'nanos');
ok(fs.readFileSync(p, 'utf8') === 'nanos', 'fs write/read round-trip');
fs.renameSync(p, p + '.2'); ok(fs.existsSync(p + '.2'), 'fs rename');
fs.unlinkSync(p + '.2');

// timers + promises
let timerFired = false;
setTimeout(() => { timerFired = true; }, 50);

// tcp loopback echo
const srv = net.createServer(s => s.pipe(s));
srv.listen(0, '127.0.0.1', () => {
  const c = net.connect(srv.address().port, '127.0.0.1', () => c.write('ping'));
  c.on('data', d => {
    ok(d.toString() === 'ping', 'tcp loopback echo');
    c.end(); srv.close();
    dns.lookup('localhost', (err, addr) => {
      ok(!err && addr, 'dns lookup localhost');
      Promise.resolve(41).then(v => {
        ok(v + 1 === 42, 'promise resolves');
        ok(timerFired, 'timer fired');
        console.log(failures ? 'node-smoke: FAIL' : 'node-smoke: PASS');
        process.exit(failures ? 1 : 0);
      });
    });
  });
});
```

- [ ] Stage it at `/disks/main/apps/node-smoke/node-smoke.js` (image staging rule).
- [ ] Fix what fails, in this order: `fs` basics, timers, `net` loopback, `dns` (`localhost` may
  resolve from `/etc/hosts`-equivalent — record how NanOS resolves it).
- [ ] `worker_threads`: run `node.nxe -e "new (require('worker_threads').Worker)('', {eval:true})"`
  once; record works/fails in `docs/en/node-port.md`. Electron's usage decides later whether to
  fix — do not sink time now.
- [ ] Gate:

```sh
node.nxe /disks/main/apps/node-smoke/node-smoke.js   # -> "node-smoke: PASS", exit 0
```

## Task 2.5: Native Addon Mechanism (Static Registration)

Per Decisions: no `.node` dlopen. Prove the static path end to end.

- [ ] Create the tiny addon in-repo at `ports/node/addon-smoke/`:

```cpp
// addon.cc — smallest possible linked addon: exports add(a, b).
#include <node.h>
namespace {
void Add(const v8::FunctionCallbackInfo<v8::Value>& args) {
  double a = args[0].As<v8::Number>()->Value();
  double b = args[1].As<v8::Number>()->Value();
  args.GetReturnValue().Set(a + b);
}
void Init(v8::Local<v8::Object> exports, v8::Local<v8::Value>, void*) {
  NODE_SET_METHOD(exports, "add", Add);
}
}  // namespace
NODE_MODULE_LINKED(addon_smoke, Init)
```

- [ ] Build node with `--link-module ./ports/node/addon-smoke/addon.cc` (wire into
  `build-node.sh`; check the exact linked-module configure syntax for the pinned version and
  record it).
- [ ] Smoke inside NanOS:

```sh
node.nxe -e "console.log(process._linkedBinding('addon_smoke').add(40, 2))"
```

Expected output: `42`.

- [ ] Document the policy in `docs/en/node-port.md`: how an Electron app's required native module
  gets compiled and linked into the runtime; `.ndl`-backed `process.dlopen` is a follow-up.

## Task 2.6: Smoke Script And Test Subset

- [ ] Write `scripts/smoke-node.sh` in the QEMU house style (EXECUTION_GUIDE): boot image,
  log in, run `node.nxe /disks/main/apps/node-smoke/node-smoke.js`, assert serial contains
  `node-smoke: PASS`, assert no faults, prove deliberate failure (temporarily grep for a wrong
  marker) once.
- [ ] Optional depth (only after smoke is green): run a Node upstream test subset ON NanOS is
  impractical at first; instead list in `docs/en/node-port.md` which upstream areas the smoke
  covers and which are known-untested (child_process, workers, https).
- [ ] `child_process`: verify `node.nxe -e "require('child_process').execSync('ls /')"` prints a
  listing (fork/exec exist; this mostly tests libuv spawn on NanOS). Record result.

## Gate

- [ ] `make node` builds `node.nxe` reproducibly from a clean `$SDK_WORK/node-src`.
- [ ] `node.nxe -e "console.log(1 + 2)"` prints `3` on NanOS.
- [ ] `scripts/smoke-node.sh` passes (and failed once on a deliberate bad marker).
- [ ] Linked-addon smoke prints `42`.
- [ ] `docs/en/node-port.md` has exact build commands, flag rationale, and the untested-areas list.
- [ ] `status.md`: `M2 Node` checked, log row added.
