//! rsexp — a modern (macOS-Finder-style) file explorer for NanOS, written from scratch in Rust on
//! the nwm desktop. Big colourful gradient icons, a translucent sidebar with Favorites/Locations
//! and a blue "current location" pill, and a virtual "My Computer" listing each mounted disk under
//! /disks plus Home. A thin client over the C libnwui toolkit via the reusable libnwui-rs bindings.
#![no_std]

extern crate alloc;

use core::ffi::c_void;
use alloc::vec::Vec;
use libnwui_rs::{IconItem, Node, NwNode, Ui};

extern "C" {
    fn getenv(name: *const u8) -> *const u8;
    fn nwui_dir_open(path: *const u8) -> *mut c_void;
    fn nwui_dir_next(d: *mut c_void, name: *mut u8, cap: i32, is_dir: *mut i32) -> i32;
    fn nwui_dir_close(d: *mut c_void);
}

const MUTED: u32 = 0x008a8a8e;
const SIDE_BG: u32 = 0x00f4f5f8;

/* entry kinds -> icon + behavior */
const K_DIR: u8 = 0;
const K_NXE: u8 = 1;
const K_TEXT: u8 = 2;
const K_IMAGE: u8 = 3;
const K_FILE: u8 = 4;
const K_DRIVE: u8 = 5;
const K_HOME: u8 = 6;
const K_UP: u8 = 7;

/* sidebar place kinds */
const P_HOME: u8 = 0;
const P_COMPUTER: u8 = 1;
const P_DISK: u8 = 2;

type Icon = (*const u32, i32, i32);

struct Icons {
    drive: Icon,
    folder: Icon,
    home: Icon,
    program: Icon,
    text: Icon,
    image: Icon,
    file: Icon,
}

struct App {
    ui: *mut libnwui_rs::NwUi,
    view: *mut NwNode,
    crumb: *mut NwNode,
    icons: Icons,
    my_computer: bool,
    cwd: Vec<u8>,                 // current dir (NUL-terminated) when !my_computer
    names: Vec<Vec<u8>>,
    paths: Vec<Vec<u8>>,
    kinds: Vec<u8>,
    items: Vec<IconItem>,
    item_idx: Vec<usize>,         // filtered item position -> index into names/paths/kinds
    // sidebar places
    place_nodes: Vec<*mut NwNode>,
    place_paths: Vec<Vec<u8>>,    // NUL-terminated; empty for My Computer
    place_kind: Vec<u8>,
    // navigation history (each entry: empty = My Computer, else a path without NUL) + cursor
    hist: Vec<Vec<u8>>,
    hpos: i32,
    // live search filter (NUL-terminated buffer the search field writes into)
    query: [u8; 64],
    search: *mut NwNode,          // the search text field (so navigation can clear it)
    // SQLite-backed recursive search: a :memory: index, (re)built per location on first search.
    #[cfg(feature = "sqlite")]
    db: *mut c_void,              // sqlite3* (lazily opened), null until the first search
    #[cfg(feature = "sqlite")]
    indexed_root: Vec<u8>,        // the path the in-memory index currently covers (no NUL)
}

fn ends_with(s: &[u8], suf: &[u8]) -> bool {
    s.len() >= suf.len() && &s[s.len() - suf.len()..] == suf
}

fn classify(name: &[u8]) -> u8 {
    if ends_with(name, b".nxe") { return K_NXE; }
    if ends_with(name, b".png") { return K_IMAGE; }
    for ext in [b".txt".as_ref(), b".c", b".h", b".md", b".cfg", b".rs", b".sh", b".conf"] {
        if ends_with(name, ext) { return K_TEXT; }
    }
    K_FILE
}

fn starts_with(s: &[u8], pre: &[u8]) -> bool {
    s.len() >= pre.len() && &s[..pre.len()] == pre
}

impl App {
    fn icon_for(&self, kind: u8) -> Icon {
        match kind {
            K_DIR | K_UP => self.icons.folder,
            K_NXE => self.icons.program,
            K_TEXT => self.icons.text,
            K_IMAGE => self.icons.image,
            K_DRIVE => self.icons.drive,
            K_HOME => self.icons.home,
            _ => self.icons.file,
        }
    }

    fn clear(&mut self) {
        self.names.clear();
        self.paths.clear();
        self.kinds.clear();
        self.items.clear();
    }

    /// Drop any active search text (model + the search field's contents) — called whenever we load
    /// a real location, so navigating into a result (or anywhere) leaves the plain directory view.
    fn clear_search(&mut self) {
        self.query[0] = 0;
        if !self.search.is_null() {
            Node(self.search).set_text("");
        }
    }

    fn push(&mut self, label: &[u8], path: &[u8], kind: u8) {
        let mut nm = Vec::with_capacity(label.len() + 1);
        nm.extend_from_slice(label);
        nm.push(0);
        let mut pt = Vec::with_capacity(path.len() + 1);
        pt.extend_from_slice(path);
        pt.push(0);
        self.names.push(nm);
        self.paths.push(pt);
        self.kinds.push(kind);
    }

    /// Rebuild the (possibly filtered) icon list from the model + current search query.
    fn apply_filter(&mut self) {
        self.items.clear();
        self.item_idx.clear();
        let qlen = cstr_len(&self.query);
        for i in 0..self.names.len() {
            let nm = &self.names[i];
            let nlen = nm.len() - 1;            // drop the NUL
            if qlen == 0 || contains_ci(&nm[..nlen], &self.query[..qlen]) {
                let (icon, iw, ih) = self.icon_for(self.kinds[i]);
                self.items.push(IconItem { label: nm.as_ptr(), icon, iw, ih });
                self.item_idx.push(i);
            }
        }
        Node(self.view).iconview_set(&self.items);
    }

    fn commit(&mut self) {
        self.apply_filter();
        self.update_sidebar();
    }

    /// Re-filter when the search text changes (model unchanged).
    fn refilter(&mut self) {
        self.apply_filter();
    }

    /* ---- navigation history --------------------------------------------------------- */
    fn record(&mut self, entry: Vec<u8>) {
        let keep = (self.hpos + 1) as usize;
        if keep < self.hist.len() {
            self.hist.truncate(keep);          // drop the forward branch
        }
        self.hist.push(entry);
        self.hpos = self.hist.len() as i32 - 1;
    }

    /// Navigate to `entry` (empty = My Computer, else a path); push history when `record`.
    fn go(&mut self, entry: &[u8], record: bool) {
        let ok = if entry.is_empty() {
            self.load_my_computer();
            true
        } else {
            self.load_dir(entry)
        };
        if ok && record {
            self.record(entry.to_vec());
        }
    }

    fn back(&mut self) {
        if self.hpos > 0 {
            self.hpos -= 1;
            let e = self.hist[self.hpos as usize].clone();
            self.go(&e, false);
        }
    }
    fn forward(&mut self) {
        if (self.hpos as usize) + 1 < self.hist.len() {
            self.hpos += 1;
            let e = self.hist[self.hpos as usize].clone();
            self.go(&e, false);
        }
    }

    fn set_crumb(&self, s: &[u8]) {
        Node(self.crumb).set_text(unsafe { core::str::from_utf8_unchecked(s) });
    }

    /// Highlight the sidebar row matching the current location (longest path-prefix match).
    fn update_sidebar(&self) {
        if self.place_nodes.is_empty() {
            return;
        }
        let cwd = if self.my_computer { &b""[..] } else { &self.cwd[..self.cwd.len() - 1] };
        let mut best = usize::MAX;
        let mut best_len = 0usize;
        for i in 0..self.place_nodes.len() {
            let active = if self.my_computer {
                self.place_kind[i] == P_COMPUTER
            } else {
                let pp = &self.place_paths[i];
                if self.place_kind[i] != P_COMPUTER && !pp.is_empty() {
                    let p = &pp[..pp.len() - 1];
                    starts_with(cwd, p) && p.len() >= best_len
                } else {
                    false
                }
            };
            if active {
                if self.my_computer {
                    best = i;
                    break;
                }
                let pp = &self.place_paths[i];
                best_len = pp.len() - 1;
                best = i;
            }
        }
        for i in 0..self.place_nodes.len() {
            Node(self.place_nodes[i]).set_active(i == best);
        }
    }

    fn load_my_computer(&mut self) {
        self.my_computer = true;
        self.clear_search();
        self.clear();
        let d = unsafe { nwui_dir_open(b"/disks\0".as_ptr()) };
        if !d.is_null() {
            let mut name = [0u8; 256];
            let mut is_dir = 0i32;
            while unsafe { nwui_dir_next(d, name.as_mut_ptr(), 256, &mut is_dir) } == 1 {
                let n = cstr_len(&name);
                let mut path = Vec::new();
                path.extend_from_slice(b"/disks/");
                path.extend_from_slice(&name[..n]);
                self.push(&name[..n], &path, K_DRIVE);
            }
            unsafe { nwui_dir_close(d) }
        }
        let home = home_dir();
        self.push(b"Home", &home, K_HOME);
        self.set_crumb(b"My Computer");
        self.commit();
    }

    fn load_dir(&mut self, path: &[u8]) -> bool {
        let d = unsafe { nwui_dir_open(nul(path).as_ptr()) };
        if d.is_null() {
            return false;
        }
        self.my_computer = false;
        self.cwd = nul(path);
        self.clear_search();
        self.clear();
        if !(path.len() == 1 && path[0] == b'/') {
            self.push(b"..", path, K_UP);
        }
        let mut name = [0u8; 256];
        let mut is_dir = 0i32;
        while unsafe { nwui_dir_next(d, name.as_mut_ptr(), 256, &mut is_dir) } == 1 {
            let n = cstr_len(&name);
            let mut full = Vec::new();
            full.extend_from_slice(path);
            if !(full.len() == 1 && full[0] == b'/') {
                full.push(b'/');
            }
            full.extend_from_slice(&name[..n]);
            let kind = if is_dir != 0 { K_DIR } else { classify(&name[..n]) };
            self.push(&name[..n], &full, kind);
        }
        unsafe { nwui_dir_close(d) }
        let crumb = self.cwd[..self.cwd.len() - 1].to_vec();
        self.set_crumb(&crumb);
        self.commit();
        true
    }

    fn nav_up(&mut self) {
        if self.my_computer {
            return;
        }
        let cwd = self.cwd[..self.cwd.len() - 1].to_vec();
        let par = parent_of(&cwd);
        if par == b"/disks" || par.is_empty() {
            self.go(b"", true);
        } else {
            let p = par.to_vec();
            self.go(&p, true);
        }
    }

    fn activate(&mut self) {
        let sel = Node(self.view).iconview_selected();
        if sel < 0 || sel as usize >= self.item_idx.len() {
            return;
        }
        let i = self.item_idx[sel as usize];
        match self.kinds[i] {
            K_UP => self.nav_up(),
            K_DRIVE | K_HOME | K_DIR => {
                let p = self.paths[i][..self.paths[i].len() - 1].to_vec();
                self.go(&p, true);
            }
            K_NXE => {
                let p = self.paths[i].clone();
                Ui(self.ui).spawn(unsafe { core::str::from_utf8_unchecked(&p[..p.len() - 1]) });
            }
            _ => {}
        }
    }

    fn go_place(&mut self, node: *mut NwNode) {
        let mut idx = usize::MAX;
        for i in 0..self.place_nodes.len() {
            if self.place_nodes[i] == node {
                idx = i;
                break;
            }
        }
        if idx == usize::MAX {
            return;
        }
        if self.place_kind[idx] == P_COMPUTER {
            self.go(b"", true);
        } else {
            let p = self.place_paths[idx][..self.place_paths[idx].len() - 1].to_vec();
            self.go(&p, true);
        }
    }

    /// Restore the current location (used when the search box is cleared).
    fn reload_location(&mut self) {
        let e = if self.hpos >= 0 { self.hist[self.hpos as usize].clone() } else { Vec::new() };
        self.go(&e, false);
    }

    #[cfg(not(feature = "sqlite"))]
    fn do_search(&mut self) {
        self.refilter();
    }

    /// SQLite-backed recursive search: index the subtree under the current location (lazily, into
    /// an in-memory DB) and show every file whose name matches, from anywhere in that subtree.
    #[cfg(feature = "sqlite")]
    fn do_search(&mut self) {
        let qlen = cstr_len(&self.query);
        if qlen == 0 {
            self.reload_location();          // empty box -> back to the plain directory view
            return;
        }
        let root = if self.my_computer {
            b"/disks".to_vec()
        } else {
            self.cwd[..self.cwd.len() - 1].to_vec()
        };
        self.sql_index(&root);               // (re)build the index for this subtree if needed
        let n = self.sql_search(qlen);       // replace the model with the SQL result rows
        // crumb: "Search 'q' — N"
        let mut c = Vec::new();
        c.extend_from_slice(b"Search '");
        c.extend_from_slice(&self.query[..qlen]);
        c.extend_from_slice(b"' \xe2\x80\x94 ");      // em dash
        push_int(&mut c, n);
        self.set_crumb(&c);
        self.apply_filter();                 // results already match the query -> all shown
    }
}

/* ---- SQLite-backed recursive search (feature = "sqlite", links libsqlite.ndl) ---- */
#[cfg(feature = "sqlite")]
mod sqlite_ffi {
    use core::ffi::c_void;
    pub const SQLITE_ROW: i32 = 100;
    // SQLITE_TRANSIENT: tell sqlite to COPY the bound text, so our temporary buffers can drop.
    pub fn transient() -> *mut c_void { (!0usize) as *mut c_void }
    extern "C" {
        pub fn sqlite3_open(path: *const u8, db: *mut *mut c_void) -> i32;
        pub fn sqlite3_exec(db: *mut c_void, sql: *const u8, cb: *mut c_void, arg: *mut c_void,
                            err: *mut *mut u8) -> i32;
        pub fn sqlite3_prepare_v2(db: *mut c_void, sql: *const u8, n: i32,
                                  stmt: *mut *mut c_void, tail: *mut *const u8) -> i32;
        pub fn sqlite3_bind_text(stmt: *mut c_void, idx: i32, text: *const u8, n: i32,
                                 destructor: *mut c_void) -> i32;
        pub fn sqlite3_bind_int(stmt: *mut c_void, idx: i32, v: i32) -> i32;
        pub fn sqlite3_step(stmt: *mut c_void) -> i32;
        pub fn sqlite3_column_text(stmt: *mut c_void, col: i32) -> *const u8;
        pub fn sqlite3_column_int(stmt: *mut c_void, col: i32) -> i32;
        pub fn sqlite3_reset(stmt: *mut c_void) -> i32;
        pub fn sqlite3_finalize(stmt: *mut c_void) -> i32;
    }
}

#[cfg(feature = "sqlite")]
impl App {
    /// Ensure the in-memory index covers `root` (the current location's subtree). Rebuilt only when
    /// the location changes; a recursive walk (depth- and count-bounded) inserts one row per file.
    fn sql_index(&mut self, root: &[u8]) {
        use sqlite_ffi::*;
        if self.db.is_null() {
            let mut db: *mut c_void = core::ptr::null_mut();
            if unsafe { sqlite3_open(b":memory:\0".as_ptr(), &mut db) } != 0 { return; }
            self.db = db;
        }
        if self.indexed_root == root { return; }   // already indexed this subtree
        unsafe {
            sqlite3_exec(self.db,
                b"DROP TABLE IF EXISTS files; CREATE TABLE files(name TEXT, path TEXT, kind INT);\0".as_ptr(),
                core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut());
        }
        let mut ins: *mut c_void = core::ptr::null_mut();
        if unsafe { sqlite3_prepare_v2(self.db,
            b"INSERT INTO files(name,path,kind) VALUES(?,?,?);\0".as_ptr(), -1,
            &mut ins, core::ptr::null_mut()) } != 0 { return; }
        let mut count = 0i32;
        self.walk_index(ins, root, 0, &mut count);
        unsafe { sqlite3_finalize(ins); }
        self.indexed_root = root.to_vec();
    }

    fn walk_index(&self, ins: *mut c_void, dir: &[u8], depth: i32, count: &mut i32) {
        use sqlite_ffi::*;
        if depth > 16 || *count > 8000 { return; }
        let d = unsafe { nwui_dir_open(nul(dir).as_ptr()) };
        if d.is_null() { return; }
        let mut name = [0u8; 256];
        let mut is_dir = 0i32;
        while unsafe { nwui_dir_next(d, name.as_mut_ptr(), 256, &mut is_dir) } == 1 {
            let n = cstr_len(&name);
            if n == 0 { continue; }
            let mut full = Vec::new();
            full.extend_from_slice(dir);
            if !(full.len() == 1 && full[0] == b'/') { full.push(b'/'); }
            full.extend_from_slice(&name[..n]);
            let kind = if is_dir != 0 { K_DIR } else { classify(&name[..n]) };
            let nm = nul(&name[..n]);
            let ft = nul(&full);
            unsafe {
                sqlite3_reset(ins);
                sqlite3_bind_text(ins, 1, nm.as_ptr(), -1, transient());
                sqlite3_bind_text(ins, 2, ft.as_ptr(), -1, transient());
                sqlite3_bind_int(ins, 3, kind as i32);
                sqlite3_step(ins);
            }
            *count += 1;
            if is_dir != 0 { self.walk_index(ins, &full, depth + 1, count); }
            if *count > 8000 { break; }
        }
        unsafe { nwui_dir_close(d) }
    }

    /// Run the LIKE query and replace the model with the matching rows. Returns the row count.
    fn sql_search(&mut self, qlen: usize) -> i32 {
        use sqlite_ffi::*;
        self.clear();
        if self.db.is_null() { return 0; }
        let mut st: *mut c_void = core::ptr::null_mut();
        if unsafe { sqlite3_prepare_v2(self.db,
            b"SELECT name,path,kind FROM files WHERE name LIKE ? ORDER BY kind, name LIMIT 2000;\0".as_ptr(),
            -1, &mut st, core::ptr::null_mut()) } != 0 { return 0; }
        let mut pat = Vec::with_capacity(qlen + 3);
        pat.push(b'%');
        pat.extend_from_slice(&self.query[..qlen]);
        pat.push(b'%');
        pat.push(0);
        unsafe { sqlite3_bind_text(st, 1, pat.as_ptr(), -1, transient()); }
        let mut count = 0i32;
        loop {
            if unsafe { sqlite3_step(st) } != SQLITE_ROW { break; }
            let nm = unsafe { sqlite3_column_text(st, 0) };
            let pt = unsafe { sqlite3_column_text(st, 1) };
            let kind = unsafe { sqlite3_column_int(st, 2) } as u8;
            let nmv = cstr_from(nm);
            let ptv = cstr_from(pt);
            self.push(&nmv, &ptv, kind);
            count += 1;
        }
        unsafe { sqlite3_finalize(st) };
        count
    }
}

/* ---- small no_std helpers ---- */
fn cstr_len(b: &[u8]) -> usize {
    let mut i = 0;
    while i < b.len() && b[i] != 0 {
        i += 1;
    }
    i
}

fn nul(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s);
    if v.last() != Some(&0) {
        v.push(0);
    }
    v
}

/// Append the decimal digits of a non-negative integer to `out`.
#[cfg(feature = "sqlite")]
fn push_int(out: &mut Vec<u8>, mut v: i32) {
    if v <= 0 { out.push(b'0'); return; }
    let mut tmp = [0u8; 12];
    let mut i = 0;
    while v > 0 { tmp[i] = b'0' + (v % 10) as u8; v /= 10; i += 1; }
    while i > 0 { i -= 1; out.push(tmp[i]); }
}

/// Read a C (NUL-terminated) string from a raw pointer into an owned, NUL-free byte vec.
#[cfg(feature = "sqlite")]
fn cstr_from(p: *const u8) -> Vec<u8> {
    let mut v = Vec::new();
    if p.is_null() { return v; }
    let mut i = 0isize;
    loop {
        let c = unsafe { *p.offset(i) };
        if c == 0 { break; }
        v.push(c);
        i += 1;
    }
    v
}

fn home_dir() -> Vec<u8> {
    let p = unsafe { getenv(b"HOME\0".as_ptr()) };
    if !p.is_null() {
        let mut v = Vec::new();
        let mut i = 0isize;
        loop {
            let c = unsafe { *p.offset(i) };
            if c == 0 {
                break;
            }
            v.push(c);
            i += 1;
        }
        if !v.is_empty() {
            return v;
        }
    }
    b"/disks/main".to_vec()
}

fn parent_of(path: &[u8]) -> &[u8] {
    let mut i = path.len();
    while i > 0 && path[i - 1] != b'/' {
        i -= 1;
    }
    if i <= 1 {
        return &path[..0];
    }
    &path[..i - 1]
}

/* ---- callbacks (raw C ABI; `user` is the leaked *mut App) ---- */
extern "C" fn cb_activate(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).activate() }
}
extern "C" fn cb_noop(_n: *mut NwNode, _user: *mut c_void) {}
extern "C" fn cb_up(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).nav_up() }
}
extern "C" fn cb_homebtn(_n: *mut NwNode, user: *mut c_void) {
    unsafe {
        let app = &mut *(user as *mut App);
        let h = home_dir();
        app.go(&h, true);
    }
}
extern "C" fn cb_place(n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).go_place(n) }
}
extern "C" fn cb_mycomputer(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).go(b"", true) }
}
extern "C" fn cb_back(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).back() }
}
extern "C" fn cb_forward(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).forward() }
}
extern "C" fn cb_search(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).do_search() }
}
extern "C" fn cb_close(_n: *mut NwNode, _user: *mut c_void) {
    unsafe { libnwui_rs::exit(0) }
}

/// Case-insensitive substring test (ASCII).
fn contains_ci(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    fn lc(b: u8) -> u8 { if b >= b'A' && b <= b'Z' { b + 32 } else { b } }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        let mut j = 0;
        while j < needle.len() && lc(hay[i + j]) == lc(needle[j]) {
            j += 1;
        }
        if j == needle.len() {
            return true;
        }
        i += 1;
    }
    false
}

fn load_icon(path: &str, fallback: Icon) -> Icon {
    match libnwui_rs::load_png(path) {
        Some(t) => t,
        None => fallback,
    }
}

#[no_mangle]
pub extern "C" fn main() -> i32 {
    let ui = match Ui::open("Files", 620, 420) {
        Some(u) => u,
        None => return 1,
    };

    let nothing: Icon = (core::ptr::null(), 0, 0);
    let file = load_icon("/disks/main/nanos/share/icons/file.png", nothing);
    let icons = Icons {
        drive: load_icon("/disks/main/nanos/share/icons/drive.png", file),
        folder: load_icon("/disks/main/nanos/share/icons/folder.png", file),
        home: load_icon("/disks/main/nanos/share/icons/home.png", file),
        program: load_icon("/disks/main/nanos/share/icons/program.png", file),
        text: load_icon("/disks/main/nanos/share/icons/text.png", file),
        image: load_icon("/disks/main/nanos/share/icons/image.png", file),
        file,
    };

    let ic_back = load_icon("/disks/main/nanos/share/icons/ui-back.png", nothing);
    let ic_fwd = load_icon("/disks/main/nanos/share/icons/ui-fwd.png", nothing);
    let ic_up = load_icon("/disks/main/nanos/share/icons/ui-up.png", nothing);
    let ic_home = load_icon("/disks/main/nanos/share/icons/ui-home.png", nothing);

    let crumb = ui.label("My Computer");

    let app = alloc::boxed::Box::new(App {
        ui: ui.0,
        view: core::ptr::null_mut(),
        crumb: crumb.0,
        icons,
        my_computer: true,
        cwd: alloc::vec![b'/', 0],
        names: Vec::new(),
        paths: Vec::new(),
        kinds: Vec::new(),
        items: Vec::new(),
        item_idx: Vec::new(),
        place_nodes: Vec::new(),
        place_paths: Vec::new(),
        place_kind: Vec::new(),
        hist: Vec::new(),
        hpos: -1,
        query: [0u8; 64],
        search: core::ptr::null_mut(),
        #[cfg(feature = "sqlite")]
        db: core::ptr::null_mut(),
        #[cfg(feature = "sqlite")]
        indexed_root: Vec::new(),
    });
    let app_ptr = alloc::boxed::Box::into_raw(app) as *mut c_void;
    let view = ui.iconview_raw(cb_activate, cb_noop, app_ptr);
    unsafe { (&mut *(app_ptr as *mut App)).view = view.0; }

    // global menu (shown in the system menu bar when Files is focused)
    let mfile = ui.menu("File");
    ui.menu_item(mfile, "Close", cb_close, app_ptr);
    let mgo = ui.menu("Go");
    ui.menu_item(mgo, "My Computer", cb_mycomputer, app_ptr);
    ui.menu_item(mgo, "Home", cb_homebtn, app_ptr);
    ui.menu_item(mgo, "Up", cb_up, app_ptr);

    // ---- sidebar: Favorites + Locations, place rows with a "current location" pill ----
    let sidebar = ui.vbox();
    sidebar.add(ui.label("FAVORITES").colors(MUTED, 0));
    let home_row = ui.link_raw("Home", cb_place, app_ptr);
    sidebar.add(home_row);
    sidebar.add(ui.label("LOCATIONS").colors(MUTED, 0));
    let comp_row = ui.link_raw("My Computer", cb_place, app_ptr);
    sidebar.add(comp_row);

    // register the fixed places (Home favorite + My Computer), then one row per mounted disk
    unsafe {
        let a = &mut *(app_ptr as *mut App);
        a.place_nodes.push(home_row.0);
        a.place_paths.push(nul(&home_dir()));
        a.place_kind.push(P_HOME);
        a.place_nodes.push(comp_row.0);
        a.place_paths.push(alloc::vec![0u8]);
        a.place_kind.push(P_COMPUTER);

        let d = nwui_dir_open(b"/disks\0".as_ptr());
        if !d.is_null() {
            let mut name = [0u8; 256];
            let mut is_dir = 0i32;
            while nwui_dir_next(d, name.as_mut_ptr(), 256, &mut is_dir) == 1 {
                let n = cstr_len(&name);
                let row = ui.link_raw(core::str::from_utf8_unchecked(&name[..n]), cb_place, app_ptr);
                sidebar.add(row);
                let mut p = Vec::new();
                p.extend_from_slice(b"/disks/");
                p.extend_from_slice(&name[..n]);
                p.push(0);
                a.place_nodes.push(row.0);
                a.place_paths.push(p);
                a.place_kind.push(P_DISK);
            }
            nwui_dir_close(d);
        }
    }
    let sidebar = sidebar.gap(4).pad(12).colors(0, SIDE_BG).size(200, 0);

    // search field over the app's query buffer (stable address: App is leaked)
    let qbuf = unsafe { (*(app_ptr as *mut App)).query.as_mut_ptr() };
    let search = ui.textfield(qbuf, 64, cb_search, app_ptr);
    unsafe { (&mut *(app_ptr as *mut App)).search = search.0; }   // so navigation can clear it

    let toolbar = ui.hbox()
        .add(ui.iconbtn(ic_back, cb_back, app_ptr))
        .add(ui.iconbtn(ic_fwd, cb_forward, app_ptr))
        .add(ui.iconbtn(ic_up, cb_up, app_ptr))
        .add(ui.iconbtn(ic_home, cb_homebtn, app_ptr))
        .add(crumb.flex(1))
        .add(search.size(160, 0))
        .gap(6)
        .pad(6)
        .colors(0x001d2733, 0x00eef2f8);   /* a defined toolbar strip */

    let body = ui.hbox().add(sidebar).add(view.flex(1));
    let root = ui.vbox().add(toolbar).add(body.flex(1));

    unsafe { (&mut *(app_ptr as *mut App)).go(b"", true); }   // initial view + history entry

    // Pre-warm the SQLite index for the opening location (/disks) so the first search is instant
    // — and, since this exercises sqlite3_open/exec/prepare/bind/step at startup, a clean launch
    // doubles as proof the libsqlite.ndl FFI works end to end.
    #[cfg(feature = "sqlite")]
    unsafe {
        let a = &mut *(app_ptr as *mut App);
        let root = if a.my_computer { b"/disks".to_vec() } else { a.cwd[..a.cwd.len() - 1].to_vec() };
        a.sql_index(&root);
    }

    ui.focus(view);
    ui.run(root);
    0
}
