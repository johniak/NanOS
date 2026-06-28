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
    sort_key: i32,                // SORT_* applied to the current view
    // file-management state
    clip_path: Vec<u8>,           // copy/cut source (NUL-terminated); empty = clipboard empty
    clip_cut: bool,               // true = cut (move on paste), false = copy
    rename_from: Vec<u8>,         // rename source path (NUL) while the rename dialog is open
    pending: Vec<u8>,             // delete target (NUL) while the confirm dialog is open
    dlg_buf: [u8; 256],           // text-input buffer for the rename / new-folder dialogs
    addr: [u8; 512],              // editable address-bar buffer (reflects the current path)
    status: *mut NwNode,          // the status-bar label
    // SQLite-backed recursive search: a :memory: index, (re)built per location on first search.
    #[cfg(feature = "sqlite")]
    db: *mut c_void,              // sqlite3* (lazily opened), null until the first search
    #[cfg(feature = "sqlite")]
    indexed_root: Vec<u8>,        // the path the in-memory index currently covers (no NUL)
    // persisted per-directory sort: a file-backed SQLite DB (survives reboot).
    #[cfg(feature = "sqlite")]
    prefs_db: *mut c_void,        // sqlite3* for ~/.rsexp.db (lazily opened), null until used
}

const SORT_NAME: i32 = 0;         // A -> Z (case-insensitive)
const SORT_NAME_DESC: i32 = 1;    // Z -> A
const SORT_TYPE: i32 = 2;         // folders first, then by name

/* normalized scancodes for keyboard accelerators (see nwui_core.h) */
const SC_F2: i32 = 0x3C;          // Rename
const SC_F5: i32 = 0x3F;          // Refresh
const SC_DEL: i32 = 0xD3;         // Delete (extended; bit7 set)

fn is_dirish(kind: u8) -> bool { matches!(kind, K_DIR | K_DRIVE | K_HOME | K_UP) }

/// Case-insensitive ASCII byte-string compare of two NUL-terminated names.
fn name_cmp(a: &[u8], b: &[u8]) -> core::cmp::Ordering {
    fn lc(c: u8) -> u8 { if c >= b'A' && c <= b'Z' { c + 32 } else { c } }
    let mut i = 0;
    loop {
        let (ca, cb) = (a.get(i).copied().unwrap_or(0), b.get(i).copied().unwrap_or(0));
        if ca == 0 || cb == 0 { return ca.cmp(&cb); }
        let (la, lb) = (lc(ca), lc(cb));
        if la != lb { return la.cmp(&lb); }
        i += 1;
    }
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
        self.update_status();
    }

    /// Re-filter when the search text changes (model unchanged). Used by the non-SQLite search
    /// path; with the `sqlite` feature, search replaces the model instead, so this goes unused.
    #[allow(dead_code)]
    fn refilter(&mut self) {
        self.apply_filter();
    }

    /* ---- sorting (persisted per directory) -------------------------------------------- */
    /// The path used as the persistence key for the current location.
    fn loc_key(&self) -> Vec<u8> {
        if self.my_computer { b"/disks".to_vec() } else { self.cwd[..self.cwd.len() - 1].to_vec() }
    }

    /// Reorder the model (names/paths/kinds in parallel) by `self.sort_key`. ".." stays first;
    /// SORT_TYPE groups folders before files. No UI refresh (the caller re-renders).
    fn sort_model(&mut self) {
        let n = self.names.len();
        if n < 2 { return; }
        let mut idx: Vec<usize> = (0..n).collect();
        let key = self.sort_key;
        idx.sort_by(|&a, &b| {
            use core::cmp::Ordering;
            let (ka, kb) = (self.kinds[a], self.kinds[b]);
            let (ua, ub) = (ka == K_UP, kb == K_UP);     // ".." is always first
            if ua != ub { return if ua { Ordering::Less } else { Ordering::Greater }; }
            let (na, nb) = (&self.names[a], &self.names[b]);
            match key {
                SORT_NAME_DESC => name_cmp(nb, na),
                SORT_TYPE => {
                    let (da, db) = (is_dirish(ka), is_dirish(kb));
                    if da != db { if da { Ordering::Less } else { Ordering::Greater } }
                    else { name_cmp(na, nb) }
                }
                _ => name_cmp(na, nb),
            }
        });
        let on = core::mem::take(&mut self.names);
        let op = core::mem::take(&mut self.paths);
        let ok = core::mem::take(&mut self.kinds);
        self.names = Vec::with_capacity(n);
        self.paths = Vec::with_capacity(n);
        self.kinds = Vec::with_capacity(n);
        for &i in &idx {
            self.names.push(on[i].clone());
            self.paths.push(op[i].clone());
            self.kinds.push(ok[i]);
        }
    }

    fn apply_sort(&mut self) {
        self.sort_model();
        self.apply_filter();
    }

    /// Change the sort, persist it for this directory, and re-render (context-menu action).
    fn set_sort(&mut self, key: i32) {
        self.sort_key = key;
        self.save_sort();
        self.apply_sort();
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

    /// Reflect the current location in the editable address bar.
    fn set_crumb(&mut self, s: &[u8]) {
        if self.crumb.is_null() { return; }
        let n = s.len().min(self.addr.len() - 1);
        self.addr[..n].copy_from_slice(&s[..n]);
        self.addr[n] = 0;
        Node(self.crumb).textfield_set(self.addr.as_ptr());
    }

    /// Ctrl+L: focus the address bar and select its text (type to replace, like a browser).
    fn focus_addr(&mut self) {
        if self.crumb.is_null() { return; }
        let c = Node(self.crumb);
        Ui(self.ui).focus(c);
        c.textfield_select_all();
    }

    /// Enter pressed in the address bar: navigate to the typed path (or report if it's not found).
    fn addr_go(&mut self) {
        let n = cstr_len(&self.addr);
        if n == 0 { return; }
        let mut path = self.addr[..n].to_vec();
        while path.len() > 1 && *path.last().unwrap() == b'/' { path.pop(); }   // strip trailing '/'
        if self.load_dir(&path) {
            self.record(path);                 // success: record history (load_dir set the crumb)
        } else {
            Ui(self.ui).message("Go", "Path not found.");
            if self.my_computer { let m = b"My Computer".to_vec(); self.set_crumb(&m); }
            else { let c = self.cwd[..self.cwd.len() - 1].to_vec(); self.set_crumb(&c); }
        }
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
        self.sort_key = self.load_sort();   // persisted sort for the My Computer view too
        self.sort_model();
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
        self.sort_key = self.load_sort();   // per-directory persisted sort (default A->Z)
        self.sort_model();
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
            K_TEXT => {
                // open-with: launch the Notepad (nwnote) with this file as argv[1]
                let p = self.paths[i].clone();   // NUL-terminated
                Ui(self.ui).spawn_arg("nwnote", p.as_ptr());
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

/* ---- file management (New Folder / Rename / Delete / Copy / Cut / Paste / Refresh) -------- */
impl App {
    /// The writable directory currently being browsed (None in the My Computer / search views,
    /// where there is no single target directory for New Folder / Paste).
    fn cur_dir(&self) -> Option<Vec<u8>> {
        if self.my_computer || cstr_len(&self.query) != 0 {
            return None;
        }
        Some(self.cwd[..self.cwd.len() - 1].to_vec())   // drop the NUL
    }

    /// Model index of the current selection (None for nothing selected or the ".." row).
    fn sel_model(&self) -> Option<usize> {
        let sel = Node(self.view).iconview_selected();
        if sel < 0 || sel as usize >= self.item_idx.len() {
            return None;
        }
        let i = self.item_idx[sel as usize];
        if self.kinds[i] == K_UP { None } else { Some(i) }
    }

    /// Absolute NUL-terminated path of the selected item (None if nothing / "..").
    fn sel_path(&self) -> Option<Vec<u8>> {
        self.sel_model().map(|i| self.paths[i].clone())
    }

    /// Re-read the current location (F5 / after a mutating operation).
    fn refresh(&mut self) {
        self.reload_location();
    }

    /* ---- New Folder ---- */
    fn new_folder(&mut self) {
        let ui = Ui(self.ui);
        if self.cur_dir().is_none() {
            ui.message("New Folder", "Not available here.");
            return;
        }
        self.dlg_buf = [0u8; 256];
        let def = b"New Folder";
        self.dlg_buf[..def.len()].copy_from_slice(def);
        let me = self as *mut App as *mut c_void;
        ui.prompt("New folder name:", self.dlg_buf.as_mut_ptr(), 256, cb_mk_folder, me);
    }
    fn do_new_folder(&mut self) {
        let ui = Ui(self.ui);
        let dir = match self.cur_dir() { Some(d) => d, None => return };
        let nlen = cstr_len(&self.dlg_buf);
        if nlen == 0 { return; }
        let path = join_path(&dir, &self.dlg_buf[..nlen]);
        if libnwui_rs::fs::exists(&path) {
            ui.message("New Folder", "A file with that name already exists.");
            return;
        }
        if !libnwui_rs::fs::mkdir(&path) {
            ui.message("New Folder", "Could not create the folder.");
            return;
        }
        self.refresh();
    }

    /* ---- Rename ---- */
    fn rename_selected(&mut self) {
        let path = match self.sel_path() { Some(p) => p, None => return };
        self.rename_from = path.clone();
        let base = basename(&path[..path.len() - 1]);
        self.dlg_buf = [0u8; 256];
        let m = base.len().min(255);
        self.dlg_buf[..m].copy_from_slice(&base[..m]);
        let me = self as *mut App as *mut c_void;
        Ui(self.ui).prompt("Rename to:", self.dlg_buf.as_mut_ptr(), 256, cb_do_rename, me);
    }
    fn do_rename(&mut self) {
        let ui = Ui(self.ui);
        if self.rename_from.is_empty() { return; }
        let nlen = cstr_len(&self.dlg_buf);
        if nlen == 0 { return; }
        let from = core::mem::take(&mut self.rename_from);          // NUL-terminated
        let parent = parent_of(&from[..from.len() - 1]).to_vec();
        let to = join_path(&parent, &self.dlg_buf[..nlen]);
        if to == from { return; }                                  // unchanged
        if libnwui_rs::fs::exists(&to) {
            ui.message("Rename", "A file with that name already exists.");
            return;
        }
        if !libnwui_rs::fs::rename(&from, &to) {
            ui.message("Rename", "Could not rename the item.");
            return;
        }
        self.refresh();
    }

    /* ---- Delete ---- */
    fn delete_selected(&mut self) {
        let path = match self.sel_path() { Some(p) => p, None => return };
        self.pending = path.clone();
        let base = basename(&path[..path.len() - 1]);
        let mut msg = Vec::new();
        msg.extend_from_slice(b"Delete '");
        msg.extend_from_slice(base);
        msg.extend_from_slice(b"'? This cannot be undone.");
        let s = unsafe { core::str::from_utf8_unchecked(&msg) };
        let me = self as *mut App as *mut c_void;
        Ui(self.ui).confirm("Delete", s, "Delete", cb_do_delete, me);
    }
    fn do_delete(&mut self) {
        if self.pending.is_empty() { return; }
        let p = core::mem::take(&mut self.pending);
        if !libnwui_rs::fs::remove(&p) {
            Ui(self.ui).message("Delete", "Could not delete the item.");
        }
        self.refresh();
    }

    /* ---- Copy / Cut / Paste ---- */
    fn copy_selected(&mut self, cut: bool) {
        if let Some(p) = self.sel_path() {
            self.clip_path = p;
            self.clip_cut = cut;
            self.update_status();
        }
    }
    fn paste(&mut self) {
        let ui = Ui(self.ui);
        if self.clip_path.is_empty() { return; }
        let dir = match self.cur_dir() {
            Some(d) => d,
            None => { ui.message("Paste", "Not available here."); return; }
        };
        let src = self.clip_path.clone();              // NUL-terminated
        let src_noz = &src[..src.len() - 1];
        // refuse to paste a directory into itself or one of its descendants
        if dir.len() >= src_noz.len() && &dir[..src_noz.len()] == src_noz
            && (dir.len() == src_noz.len() || dir[src_noz.len()] == b'/') {
            ui.message("Paste", "Cannot copy a folder into itself.");
            return;
        }
        let base = basename(src_noz).to_vec();
        if self.clip_cut {
            if join_path(&dir, &base) == src { return; }            // already here: no-op
            let dest = self.unique_dest(&dir, &base);
            if !libnwui_rs::fs::rename(&src, &dest) {
                ui.message("Move", "Could not move the item.");
                return;
            }
            self.clip_path.clear();
        } else {
            let dest = self.unique_dest(&dir, &base);
            if !libnwui_rs::fs::copy(&src, &dest) {
                ui.message("Copy", "Could not copy the item.");
                return;
            }
        }
        self.refresh();
    }
    /// A destination path in `dir` for `base` that does not collide ("name", "name copy", …).
    fn unique_dest(&self, dir: &[u8], base: &[u8]) -> Vec<u8> {
        let first = join_path(dir, base);
        if !libnwui_rs::fs::exists(&first) { return first; }
        let mut n = 1u32;
        loop {
            let mut nm = base.to_vec();
            nm.extend_from_slice(b" copy");
            if n > 1 { nm.push(b' '); push_uint(&mut nm, n); }
            let cand = join_path(dir, &nm);
            if !libnwui_rs::fs::exists(&cand) || n > 9999 { return cand; }
            n += 1;
        }
    }

    /* ---- drag and drop ---- */
    /// A drag gesture began on the selected cell — hand its path to the compositor as the payload.
    fn drag_start(&mut self) {
        if let Some(p) = self.sel_path() {
            Ui(self.ui).begin_drag(p.as_ptr());   // p is NUL-terminated
        }
    }

    /// The destination directory for a drop on cell `cell` (or the empty area).
    fn drop_dir(&self, cell: i32) -> Option<Vec<u8>> {
        if cell >= 0 && (cell as usize) < self.item_idx.len() {
            let i = self.item_idx[cell as usize];
            match self.kinds[i] {
                K_UP => {
                    if self.my_computer { None }
                    else { Some(parent_of(&self.cwd[..self.cwd.len() - 1]).to_vec()) }
                }
                k if is_dirish(k) => Some(self.paths[i][..self.paths[i].len() - 1].to_vec()),
                _ => self.cur_dir(),                   // dropped on a file -> into the current dir
            }
        } else {
            self.cur_dir()                             // empty area -> into the current dir
        }
    }

    /// A drop landed on the grid: move (or copy, with Ctrl) the dragged item into the target dir.
    fn do_drop(&mut self) {
        let text = Node(self.view).iconview_drop_text();
        if text.is_null() { return; }
        let src = read_cstr_nul(text);                 // NUL-terminated copy of the payload
        if src.len() <= 1 { return; }
        let cell = Node(self.view).iconview_drop_cell();
        let copy = (Node(self.view).iconview_drop_mods() & 2) != 0;   // Ctrl held -> copy
        let dir = match self.drop_dir(cell) { Some(d) => d, None => return };
        self.transfer(&src, &dir, copy);
    }

    /// Move (or copy) the item at NUL-terminated `src` into directory `dir` (no NUL), then refresh.
    fn transfer(&mut self, src: &[u8], dir: &[u8], copy: bool) {
        let ui = Ui(self.ui);
        let src_noz = &src[..src.len() - 1];
        if dir.len() >= src_noz.len() && &dir[..src_noz.len()] == src_noz
            && (dir.len() == src_noz.len() || dir[src_noz.len()] == b'/') {
            ui.message("Drop", "Cannot move a folder into itself.");
            return;
        }
        let base = basename(src_noz).to_vec();
        if !copy && join_path(dir, &base) == src { return; }   // already in this dir: no-op
        let dest = self.unique_dest(dir, &base);
        let ok = if copy { libnwui_rs::fs::copy(src, &dest) } else { libnwui_rs::fs::rename(src, &dest) };
        if !ok {
            ui.message(if copy { "Copy" } else { "Move" }, "The operation failed.");
            return;
        }
        self.refresh();
    }

    /* ---- status bar ---- */
    fn update_status(&self) {
        if self.status.is_null() { return; }
        let mut s = Vec::new();
        if self.my_computer {
            push_uint(&mut s, self.names.len() as u32);
            s.extend_from_slice(b" locations");
        } else if let Some(i) = self.sel_model() {
            let nm = &self.names[i];
            s.extend_from_slice(&nm[..nm.len() - 1]);
            s.extend_from_slice(b"  \xe2\x80\x94  ");      // em dash
            if is_dirish(self.kinds[i]) {
                s.extend_from_slice(b"Folder");
            } else {
                let sz = libnwui_rs::fs::size(&self.paths[i]);
                push_size(&mut s, if sz < 0 { 0 } else { sz as u64 });
            }
        } else {
            let mut count = 0u32;
            for &i in &self.item_idx {
                if self.kinds[i] != K_UP { count += 1; }
            }
            push_uint(&mut s, count);
            s.extend_from_slice(if count == 1 { b" item" } else { b" items" });
            let dir = nul(&self.cwd[..self.cwd.len() - 1]);
            if let Some((avail, _total)) = libnwui_rs::fs::space(&dir) {
                s.extend_from_slice(b"      ");
                push_size(&mut s, avail);
                s.extend_from_slice(b" free");
            }
        }
        Node(self.status).set_text(unsafe { core::str::from_utf8_unchecked(&s) });
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

/* ---- per-directory sort persistence (SQLite file DB ~/.rsexp.db) ---- */
#[cfg(feature = "sqlite")]
impl App {
    fn prefs_open(&mut self) {
        use sqlite_ffi::*;
        if !self.prefs_db.is_null() { return; }
        let mut path = home_dir();
        path.extend_from_slice(b"/.rsexp.db\0");
        let mut db: *mut c_void = core::ptr::null_mut();
        if unsafe { sqlite3_open(path.as_ptr(), &mut db) } != 0 { return; }
        self.prefs_db = db;
        unsafe {
            sqlite3_exec(db, b"CREATE TABLE IF NOT EXISTS dirsort(path TEXT PRIMARY KEY, sortkey INT);\0".as_ptr(),
                core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut());
        }
    }

    fn load_sort(&mut self) -> i32 {
        use sqlite_ffi::*;
        self.prefs_open();
        if self.prefs_db.is_null() { return SORT_NAME; }
        let mut st: *mut c_void = core::ptr::null_mut();
        if unsafe { sqlite3_prepare_v2(self.prefs_db,
            b"SELECT sortkey FROM dirsort WHERE path=?;\0".as_ptr(), -1, &mut st, core::ptr::null_mut()) } != 0 {
            return SORT_NAME;
        }
        let key = nul(&self.loc_key());
        unsafe { sqlite3_bind_text(st, 1, key.as_ptr(), -1, transient()); }
        let mut v = SORT_NAME;
        if unsafe { sqlite3_step(st) } == SQLITE_ROW { v = unsafe { sqlite3_column_int(st, 0) }; }
        unsafe { sqlite3_finalize(st) };
        v
    }

    fn save_sort(&mut self) {
        use sqlite_ffi::*;
        self.prefs_open();
        if self.prefs_db.is_null() { return; }
        let mut st: *mut c_void = core::ptr::null_mut();
        if unsafe { sqlite3_prepare_v2(self.prefs_db,
            b"INSERT INTO dirsort(path,sortkey) VALUES(?,?) ON CONFLICT(path) DO UPDATE SET sortkey=excluded.sortkey;\0".as_ptr(),
            -1, &mut st, core::ptr::null_mut()) } != 0 { return; }
        let key = nul(&self.loc_key());
        unsafe {
            sqlite3_bind_text(st, 1, key.as_ptr(), -1, transient());
            sqlite3_bind_int(st, 2, self.sort_key);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }
}

#[cfg(not(feature = "sqlite"))]
impl App {
    fn load_sort(&mut self) -> i32 { SORT_NAME }
    fn save_sort(&mut self) {}
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

/// Read a C string from a raw pointer into an owned, NUL-terminated byte vec.
fn read_cstr_nul(p: *const u8) -> Vec<u8> {
    let mut v = Vec::new();
    if p.is_null() { v.push(0); return v; }
    let mut i = 0isize;
    loop {
        let c = unsafe { *p.offset(i) };
        v.push(c);
        if c == 0 { break; }
        i += 1;
    }
    v
}

/// The last path component (after the final '/'), without a NUL.
fn basename(path: &[u8]) -> &[u8] {
    let mut i = path.len();
    while i > 0 && path[i - 1] != b'/' {
        i -= 1;
    }
    &path[i..]
}

/// Join `dir` + "/" + `name` into a fresh NUL-terminated path. Handles dir == "/".
fn join_path(dir: &[u8], name: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(dir.len() + name.len() + 2);
    v.extend_from_slice(dir);
    if !(dir.len() == 1 && dir[0] == b'/') {
        v.push(b'/');
    }
    v.extend_from_slice(name);
    v.push(0);
    v
}

/// Append a non-negative integer's decimal digits to `out`.
fn push_uint(out: &mut Vec<u8>, mut v: u32) {
    if v == 0 { out.push(b'0'); return; }
    let mut tmp = [0u8; 10];
    let mut i = 0;
    while v > 0 { tmp[i] = b'0' + (v % 10) as u8; v /= 10; i += 1; }
    while i > 0 { i -= 1; out.push(tmp[i]); }
}

/// Append a human-readable byte size ("512 B", "3.4 KB", "1.2 GB") to `out`.
fn push_size(out: &mut Vec<u8>, b: u64) {
    let units: [(u64, &[u8]); 4] =
        [(1, b"B"), (1024, b"KB"), (1024 * 1024, b"MB"), (1024 * 1024 * 1024, b"GB")];
    let mut idx = 0;
    let mut k = 0;
    while k < units.len() && b >= units[k].0 { idx = k; k += 1; }
    let (div, unit) = units[idx];
    if div == 1 {
        push_uint(out, b as u32);
    } else {
        let whole = b / div;
        push_uint(out, whole as u32);
        if whole < 100 {                                   // one decimal place for small magnitudes
            let frac = (b % div) * 10 / div;
            out.push(b'.');
            out.push(b'0' + frac as u8);
        }
    }
    out.push(b' ');
    out.extend_from_slice(unit);
}

/* ---- callbacks (raw C ABI; `user` is the leaked *mut App) ---- */
extern "C" fn cb_activate(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).activate() }
}
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
extern "C" fn cb_sort_name(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).set_sort(SORT_NAME) }
}
extern "C" fn cb_sort_name_desc(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).set_sort(SORT_NAME_DESC) }
}
extern "C" fn cb_sort_type(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).set_sort(SORT_TYPE) }
}
extern "C" fn cb_new_folder(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).new_folder() }
}
extern "C" fn cb_mk_folder(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).do_new_folder() }
}
extern "C" fn cb_rename(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).rename_selected() }
}
extern "C" fn cb_do_rename(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).do_rename() }
}
extern "C" fn cb_delete(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).delete_selected() }
}
extern "C" fn cb_do_delete(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).do_delete() }
}
extern "C" fn cb_copy(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).copy_selected(false) }
}
extern "C" fn cb_cut(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).copy_selected(true) }
}
extern "C" fn cb_paste(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).paste() }
}
extern "C" fn cb_refresh(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).refresh() }
}
extern "C" fn cb_changed(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).update_status() }
}
extern "C" fn cb_drag(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).drag_start() }
}
extern "C" fn cb_drop(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).do_drop() }
}
extern "C" fn cb_noop(_n: *mut NwNode, _user: *mut c_void) {}
extern "C" fn cb_addr_go(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).addr_go() }
}
extern "C" fn cb_focus_addr(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).focus_addr() }
}
/// Cmd+C / Cmd+X on the grid (clipboard COPY event): copy or cut the selected file.
extern "C" fn cb_copy_evt(_n: *mut NwNode, user: *mut c_void) {
    unsafe {
        let app = &mut *(user as *mut App);
        let cut = Node(app.view).iconview_copy_cut() != 0;
        app.copy_selected(cut);
    }
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

    let app = alloc::boxed::Box::new(App {
        ui: ui.0,
        view: core::ptr::null_mut(),
        crumb: core::ptr::null_mut(),   // the address-bar textfield is created after app_ptr
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
        sort_key: SORT_NAME,
        clip_path: Vec::new(),
        clip_cut: false,
        rename_from: Vec::new(),
        pending: Vec::new(),
        dlg_buf: [0u8; 256],
        addr: [0u8; 512],
        status: core::ptr::null_mut(),
        #[cfg(feature = "sqlite")]
        db: core::ptr::null_mut(),
        #[cfg(feature = "sqlite")]
        indexed_root: Vec::new(),
        #[cfg(feature = "sqlite")]
        prefs_db: core::ptr::null_mut(),
    });
    let app_ptr = alloc::boxed::Box::into_raw(app) as *mut c_void;
    let view = ui.iconview_raw(cb_activate, cb_changed, app_ptr);
    view.iconview_set_dnd(cb_drag, cb_drop);   // drag files out / drop files in (between windows)
    view.iconview_set_clipboard(cb_copy_evt, cb_paste);   // Cmd+C/X copy/cut, Cmd+V paste (files)
    unsafe { (&mut *(app_ptr as *mut App)).view = view.0; }

    // right-click context menu on the icon grid: open + file operations + sort (persisted per dir)
    ui.context_add("Open", cb_activate, app_ptr);
    ui.context_add("New Folder", cb_new_folder, app_ptr);
    ui.context_add("Rename", cb_rename, app_ptr);
    ui.context_add("Delete", cb_delete, app_ptr);
    ui.context_add("Cut", cb_cut, app_ptr);
    ui.context_add("Copy", cb_copy, app_ptr);
    ui.context_add("Paste", cb_paste, app_ptr);
    ui.context_add("Sort by Name", cb_sort_name, app_ptr);
    ui.context_add("Name (Z-A)", cb_sort_name_desc, app_ptr);
    ui.context_add("Sort by Type", cb_sort_type, app_ptr);
    ui.context_add("Refresh", cb_refresh, app_ptr);

    // keyboard accelerators — macOS-style Cmd (Super/⌘) everywhere. F2 rename, Del delete,
    // F5 refresh; Cmd+N new folder, Cmd+L focus the address bar. Cmd+C/X/V are NOT accelerators:
    // the compositor delivers them as clipboard COPY/CUT/PASTE events (wired below), so the same
    // Cmd shortcuts do file copy/cut/paste on the grid and text copy/paste in a focused field.
    ui.accel(false, 0, SC_F2, cb_rename, app_ptr);
    ui.accel(false, 0, SC_DEL, cb_delete, app_ptr);
    ui.accel(false, 0, SC_F5, cb_refresh, app_ptr);
    ui.accel(true, b'n', 0, cb_new_folder, app_ptr);
    ui.accel(true, b'l', 0, cb_focus_addr, app_ptr);

    // global menu (shown in the system menu bar when Files is focused)
    let mfile = ui.menu("File");
    ui.menu_item(mfile, "New Folder", cb_new_folder, app_ptr);
    ui.menu_item(mfile, "Close", cb_close, app_ptr);
    let medit = ui.menu("Edit");
    ui.menu_item(medit, "Rename", cb_rename, app_ptr);
    ui.menu_item(medit, "Delete", cb_delete, app_ptr);
    ui.menu_item(medit, "Cut", cb_cut, app_ptr);
    ui.menu_item(medit, "Copy", cb_copy, app_ptr);
    ui.menu_item(medit, "Paste", cb_paste, app_ptr);
    ui.menu_item(medit, "Refresh", cb_refresh, app_ptr);
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

    // editable address bar over the app's addr buffer: type a path + Enter to navigate there
    let abuf = unsafe { (*(app_ptr as *mut App)).addr.as_mut_ptr() };
    let crumb = ui.textfield(abuf, 512, cb_noop, app_ptr);
    crumb.textfield_set_submit(cb_addr_go);
    unsafe { (&mut *(app_ptr as *mut App)).crumb = crumb.0; }

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

    // status bar: a thin strip along the bottom (item count / selection size / free space)
    let status = ui.label("").colors(0x004a4a4f, 0x00eef2f8).pad(5);
    unsafe { (&mut *(app_ptr as *mut App)).status = status.0; }

    let body = ui.hbox().add(sidebar).add(view.flex(1));
    let root = ui.vbox().add(toolbar).add(body.flex(1)).add(status);

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
