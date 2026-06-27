//! rsexp — a Windows-XP-style file explorer for NanOS, written from scratch in Rust on the nwm
//! desktop. A big-icon grid, an XP left task pane ("Other Places" + "Details"), and a virtual
//! "My Computer" root listing each mounted disk under /disks plus the user's Home. A thin client
//! over the C libnwui toolkit via the reusable libnwui-rs bindings (icon grid, panel, PNG icons).
#![no_std]

extern crate alloc;

use core::ffi::c_void;
use alloc::vec::Vec;
use libnwui_rs::{IconItem, Node, Ui};

extern "C" {
    fn getenv(name: *const u8) -> *const u8;
    fn nwui_dir_open(path: *const u8) -> *mut c_void;
    fn nwui_dir_next(d: *mut c_void, name: *mut u8, cap: i32, is_dir: *mut i32) -> i32;
    fn nwui_dir_close(d: *mut c_void);
}

/* entry kinds -> icon + behavior */
const K_DIR: u8 = 0;
const K_NXE: u8 = 1;
const K_TEXT: u8 = 2;
const K_IMAGE: u8 = 3;
const K_FILE: u8 = 4;
const K_DRIVE: u8 = 5;
const K_HOME: u8 = 6;
const K_UP: u8 = 7;

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
    view: *mut libnwui_rs::NwNode,   // the iconview
    crumb: *mut libnwui_rs::NwNode,  // breadcrumb label
    dname: *mut libnwui_rs::NwNode,  // Details: selected name
    dkind: *mut libnwui_rs::NwNode,  // Details: selected kind
    icons: Icons,
    my_computer: bool,
    cwd: Vec<u8>,                    // current dir (NUL-terminated) when !my_computer
    names: Vec<Vec<u8>>,            // per-entry display label (NUL-terminated; items point in here)
    paths: Vec<Vec<u8>>,            // per-entry absolute target (NUL-terminated)
    kinds: Vec<u8>,
    items: Vec<IconItem>,
}

fn ends_with(s: &[u8], suf: &[u8]) -> bool {
    s.len() >= suf.len() && &s[s.len() - suf.len()..] == suf
}

/// Classify a (non-directory) entry name by extension into a kind.
fn classify(name: &[u8]) -> u8 {
    if ends_with(name, b".nxe") { return K_NXE; }
    if ends_with(name, b".png") { return K_IMAGE; }
    for ext in [b".txt".as_ref(), b".c", b".h", b".md", b".cfg", b".rs", b".sh", b".conf"] {
        if ends_with(name, ext) { return K_TEXT; }
    }
    K_FILE
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

    fn kind_label(kind: u8) -> &'static str {
        match kind {
            K_DIR => "Folder",
            K_UP => "Parent folder",
            K_NXE => "Program",
            K_TEXT => "Text document",
            K_IMAGE => "Image",
            K_DRIVE => "Local disk",
            K_HOME => "Home folder",
            _ => "File",
        }
    }

    fn clear(&mut self) {
        self.names.clear();
        self.paths.clear();
        self.kinds.clear();
        self.items.clear();
    }

    /// Append one entry: `label` shown under the icon, `path` the nav/spawn target, `kind` its type.
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

    /// Build the IconItem array (labels/icons) from the model and hand it to the iconview. Must run
    /// after all push()es so the name pointers are stable.
    fn commit(&mut self) {
        self.items.clear();
        for i in 0..self.names.len() {
            let (icon, iw, ih) = self.icon_for(self.kinds[i]);
            self.items.push(IconItem { label: self.names[i].as_ptr(), icon, iw, ih });
        }
        Node(self.view).iconview_set(&self.items);
        Node(self.dname).set_text("");
        Node(self.dkind).set_text("");
    }

    fn set_crumb(&self, s: &[u8]) {
        Node(self.crumb).set_text(unsafe { core::str::from_utf8_unchecked(s) });
    }

    fn load_my_computer(&mut self) {
        self.my_computer = true;
        self.clear();
        // each mounted volume under /disks -> a drive entry
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
        // Home
        let home = home_dir();
        self.push(b"Home", &home, K_HOME);
        self.set_crumb(b"My Computer");
        self.commit();
    }

    fn load_dir(&mut self, path: &[u8]) {
        let d = unsafe { nwui_dir_open(nul(path).as_ptr()) };
        if d.is_null() {
            return; // keep the previous listing on failure
        }
        self.my_computer = false;
        self.cwd = nul(path);
        self.clear();
        // ".." unless at the filesystem root
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
        self.set_crumb(&self.cwd[..self.cwd.len() - 1]); // drop NUL for display
        self.commit();
    }

    /// Go up one level. From a disk mount root (parent is "/disks") return to My Computer.
    fn nav_up(&mut self) {
        if self.my_computer {
            return;
        }
        let cwd = self.cwd[..self.cwd.len() - 1].to_vec(); // strip NUL
        let par = parent_of(&cwd);
        if par == b"/disks" || par.is_empty() {
            self.load_my_computer();
        } else {
            let p = par.to_vec();
            self.load_dir(&p);
        }
    }

    fn activate(&mut self) {
        let sel = Node(self.view).iconview_selected();
        if sel < 0 || sel as usize >= self.kinds.len() {
            return;
        }
        let i = sel as usize;
        match self.kinds[i] {
            K_UP => self.nav_up(),
            K_DRIVE | K_HOME | K_DIR => {
                let p = self.paths[i][..self.paths[i].len() - 1].to_vec();
                self.load_dir(&p);
            }
            K_NXE => {
                let p = self.paths[i].clone();
                Ui(self.ui).spawn(unsafe { core::str::from_utf8_unchecked(&p[..p.len() - 1]) });
            }
            _ => {}
        }
    }

    fn selection_changed(&mut self) {
        let sel = Node(self.view).iconview_selected();
        if sel < 0 || sel as usize >= self.kinds.len() {
            Node(self.dname).set_text("");
            Node(self.dkind).set_text("");
            return;
        }
        let i = sel as usize;
        let nm = &self.names[i];
        Node(self.dname).set_text(unsafe { core::str::from_utf8_unchecked(&nm[..nm.len() - 1]) });
        Node(self.dkind).set_text(Self::kind_label(self.kinds[i]));
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

/// NUL-terminate a byte slice into an owned buffer.
fn nul(s: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s);
    if v.last() != Some(&0) {
        v.push(0);
    }
    v
}

/// $HOME (NUL-terminated), falling back to /disks/main.
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

/// The parent path (everything up to, not including, the last '/'). "/disks/main" -> "/disks".
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
extern "C" fn cb_activate(_n: *mut libnwui_rs::NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).activate() }
}
extern "C" fn cb_change(_n: *mut libnwui_rs::NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).selection_changed() }
}
extern "C" fn cb_up(_n: *mut libnwui_rs::NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).nav_up() }
}
extern "C" fn cb_home(_n: *mut libnwui_rs::NwNode, user: *mut c_void) {
    unsafe {
        let app = &mut *(user as *mut App);
        let h = home_dir();
        app.load_dir(&h);
    }
}
extern "C" fn cb_mycomputer(_n: *mut libnwui_rs::NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).load_my_computer() }
}

fn load_icon(path: &str, fallback: Icon) -> Icon {
    match libnwui_rs::load_png(path) {
        Some(t) => t,
        None => fallback,
    }
}

/// crt0 calls main (C ABI). No args -> open the My Computer view.
#[no_mangle]
pub extern "C" fn main() -> i32 {
    let ui = match Ui::open("Files", 560, 380) {
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

    let crumb = ui.label("My Computer");
    let dname = ui.label("");
    let dkind = ui.label("");

    // leak the app state for the program's lifetime; callbacks receive this pointer
    let app = alloc::boxed::Box::new(App {
        ui: ui.0,
        view: core::ptr::null_mut(),   // filled in once the iconview exists
        crumb: crumb.0,
        dname: dname.0,
        dkind: dkind.0,
        icons,
        my_computer: true,
        cwd: alloc::vec![b'/', 0],
        names: Vec::new(),
        paths: Vec::new(),
        kinds: Vec::new(),
        items: Vec::new(),
    });
    let app_ptr = alloc::boxed::Box::into_raw(app) as *mut c_void;

    // create the iconview with the app pointer as `user`, then record it in the app
    let view = ui.iconview_raw(cb_activate, cb_change, app_ptr);
    unsafe { (&mut *(app_ptr as *mut App)).view = view.0; }

    // task pane: Other Places + Details
    let other = ui.panel("Other Places")
        .add(ui.button_raw("Home", cb_home, app_ptr))
        .add(ui.button_raw("My Computer", cb_mycomputer, app_ptr))
        .add(ui.button_raw("Up", cb_up, app_ptr));
    let details = ui.panel("Details")
        .add(dname)
        .add(dkind);
    let sidebar = ui.vbox().add(other).add(details).gap(8).size(150, 0);

    let toolbar = ui.hbox()
        .add(ui.button_raw("Up", cb_up, app_ptr))
        .add(ui.button_raw("Home", cb_home, app_ptr))
        .add(crumb)
        .gap(8);

    let body = ui.hbox()
        .add(sidebar)
        .add(view.flex(1))
        .gap(8);

    let root = ui.vbox()
        .add(toolbar)
        .add(body.flex(1))
        .pad(8)
        .gap(8);

    unsafe { (&mut *(app_ptr as *mut App)).load_my_computer(); }
    ui.run(root);
    0
}
