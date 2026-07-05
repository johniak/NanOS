#!/usr/bin/env bash
# smoke-sqlite.sh — boot smoke for the SQLite port (`make ARCH=x86_64 sqlite`). Proves the REAL
# sqlite3 CLI runs on NanOS x86_64 AND that a database on the read-write ext4 /disks/main is
# durable across a reboot (the whole VFS -> ext write + JBD2 -> ATA path, plus SQLite's own
# rollback journal + real fsync/ftruncate).
#
# It installs a tiny bash wrapper (sqlsmoke.sh) into jan's home via debugfs, then boots TWICE:
#   boot 1: create the table, report PRECOUNT (0 on a fresh DB), insert 3 rows, report POSTCOUNT=3.
#   boot 2: SAME image file (QEMU writes back to it), so PRECOUNT must now be 3 — proving the DB
#           survived the reboot. A second insert makes POSTCOUNT=6.
# Usage: scripts/smoke-sqlite.sh        (build disk/image64.img with sqlite installed first)
# Exit 0 = PASS.
set -u
IMG=disk/image64.img
PART="$IMG?offset=69206016"
MON=/tmp/nanos-sqlsmoke-qmon.sock
SER1=/tmp/nanos-sqlsmoke-serial1.log
SER2=/tmp/nanos-sqlsmoke-serial2.log
INT=/tmp/nanos-sqlsmoke-int.log
[ -f "$IMG" ] || { echo "FAIL: $IMG missing — run 'make ARCH=x86_64 image64' (after 'make ARCH=x86_64 sqlite')"; exit 2; }

# --- install the wrapper script into jan's home (punctuation lives in the FILE, so the typed
#     command is just `bash sqlsmoke.sh`). Labels every result with SQL string concat so the
#     assertions match single tokens on the serial log. ---
# Stage under the repo dir so the in-container debugfs (only $PWD -> /src is mounted) can read it.
TMP="$PWD/.sqlsmoke-tmp.sh"
cat > "$TMP" <<'SH'
#!/disks/main/apps/bash/bash.nxe
SQ=/disks/main/nanos/bin/sqlite3.nxe   # /nanos is under the disk mount, not the VFS root
DB=demo.db
echo SQLSMOKE_BEGIN
"$SQ" "$DB" "CREATE TABLE IF NOT EXISTS items(id INTEGER PRIMARY KEY, name TEXT);"
"$SQ" "$DB" "SELECT 'PRECOUNT=' || count(*) FROM items;"
"$SQ" "$DB" "INSERT INTO items(name) VALUES('alpha'),('beta'),('gamma');"
"$SQ" "$DB" "SELECT 'POSTCOUNT=' || count(*) FROM items;"
"$SQ" "$DB" "SELECT 'ROW=' || id || ':' || name FROM items ORDER BY id;"
"$SQ" "$DB" "SELECT 'VER=' || sqlite_version();"
echo SQLSMOKE_END
SH
# debugfs sees the ext partition root (= VFS /disks/main), so jan's home is /users/jan there.
docker run --rm -v "$PWD":/src -w /src nanos-build sh -c \
  "printf 'rm /users/jan/sqlsmoke.sh\nrm /users/jan/demo.db\nrm /users/jan/demo.db-journal\nwrite /src/.sqlsmoke-tmp.sh /users/jan/sqlsmoke.sh\nset_inode_field /users/jan/sqlsmoke.sh mode 0100644\n' | debugfs -w '$PART'" >/dev/null 2>&1
rm -f "$TMP"
# Confirm it actually landed (debugfs 'write' exits 0 even when the source is missing).
docker run --rm -v "$PWD":/src -w /src nanos-build sh -c \
  "printf 'ls /users/jan\n' | debugfs '$PART' 2>/dev/null" | grep -q sqlsmoke.sh \
  || { echo "FAIL: sqlsmoke.sh not present in image after write"; exit 2; }

boot_and_run() {  # $1=serial-log  $2=settle-after-cmd
  local ser="$1" settle="$2"
  rm -f "$MON" "$ser"
  pkill -9 -f "qemu-system-x86_64.*$IMG" 2>/dev/null
  qemu-system-x86_64 -cpu qemu64 -m 512 -drive file="$IMG",format=raw \
      -display none -serial file:"$ser" -monitor unix:"$MON",server,nowait \
      -no-reboot -d int,cpu_reset -D "$INT" &
  local qpid=$!
  for i in $(seq 1 45); do grep -q "nanos login:" "$ser" 2>/dev/null && break; sleep 1; done
  python3 - "$MON" "$settle" <<'PY'
import socket,time,sys
mon=sys.argv[1]; settle=float(sys.argv[2])
KM={' ':'spc','.':'dot','/':'slash','_':'shift-minus','\n':'ret'}
def kn(c):
    if c in KM: return KM[c]
    if c.isdigit(): return c
    if c.isalpha(): return ('shift-'+c.lower()) if c.isupper() else c
    return None
s=socket.socket(socket.AF_UNIX)
try: s.connect(mon)
except Exception as e: print("monitor connect failed:",e); sys.exit(0)
time.sleep(0.3)
try: s.settimeout(0.3); s.recv(65536)
except: pass
def typ(text, wait):
    for c in text:
        k=kn(c)
        if k: s.sendall(("sendkey "+k+"\n").encode()); time.sleep(0.05)
        try: s.settimeout(0.08); s.recv(4096)
        except: pass
    s.sendall(b"sendkey ret\n"); time.sleep(wait)
typ("jan", 2.5)                  # username
typ("jan", 4.0)                  # password -> bash login shell
typ("bash sqlsmoke.sh", settle)  # run the SQL wrapper (6 sqlite3 invocations)
# clean shutdown so QEMU flushes its disk write-back cache to the image file (persistence!)
s.sendall(b"quit\n"); time.sleep(0.3); s.close()
PY
  wait "$qpid" 2>/dev/null
}

echo "=== SQLite boot 1 (create + insert) ==="
boot_and_run "$SER1" 10
echo "=== SQLite boot 2 (reopen same image -> persistence) ==="
boot_and_run "$SER2" 10

PASS=1
chk(){ if grep -q "$1" "$2" 2>/dev/null; then echo "  OK  : $3"; else echo "  FAIL: $3 (missing '$1')"; PASS=0; fi; }
no(){  if grep -qE "$1" "$2" 2>/dev/null; then echo "  FAIL: $3"; grep -E "$1" "$2"|head -3|sed 's/^/        /'; PASS=0; else echo "  OK  : $3"; fi; }

echo "--- boot 1 ---"
chk "SQLSMOKE_BEGIN"   "$SER1" "wrapper ran"
chk "VER=3.46.1"       "$SER1" "real sqlite3 CLI 3.46.1 (sqlite_version())"
chk "PRECOUNT=0"       "$SER1" "fresh DB starts empty"
chk "POSTCOUNT=3"      "$SER1" "INSERT of 3 rows committed to ext4"
chk "ROW=1:alpha"      "$SER1" "SELECT returns the inserted rows"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process" "$SER1" "no faults"

echo "--- boot 2 (after reboot, SAME image) ---"
chk "PRECOUNT=3"       "$SER2" "DB PERSISTED across reboot (3 rows still on disk)"
chk "POSTCOUNT=6"      "$SER2" "second INSERT accumulates -> 6 rows"
no  "CPU EXCEPTION|KERNEL EXCEPTION|killed faulting process" "$SER2" "no faults"
if grep -qE "Triple fault" "$INT" 2>/dev/null; then echo "  FAIL: QEMU triple fault"; PASS=0; else echo "  OK  : no triple fault"; fi

# Filesystem integrity: SQLite churns rollback journals (create+delete a file per transaction),
# so a clean fsck after all that write/delete traffic proves the ext unlink/free path stays
# consistent (no orphaned inodes, no leaked blocks).
echo "--- filesystem integrity ---"
FSCK=$(docker run --rm -v "$PWD":/src -w /src nanos-build sh -c 'e2fsck -fn "'"$PART"'" 2>&1')
if echo "$FSCK" | grep -qiE "Unattached|bitmap differences|orphan|FILE SYSTEM WAS MODIFIED|still has errors"; then
	echo "  FAIL: e2fsck found inconsistencies after the SQLite workload"
	echo "$FSCK" | grep -iE "Unattached|differ|orphan|error" | head -6 | sed 's/^/        /'
	PASS=0
else
	echo "  OK  : image is e2fsck-clean after create/insert + journal churn"
fi

echo "============================="
if [ "$PASS" = 1 ]; then echo "SQLite smoke: PASS"; exit 0; else echo "SQLite smoke: FAIL"; echo "--- boot1 tail ---"; tail -25 "$SER1" 2>/dev/null; echo "--- boot2 tail ---"; tail -25 "$SER2" 2>/dev/null; exit 1; fi
