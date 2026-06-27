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
    // sidebar places
    place_nodes: Vec<*mut NwNode>,
    place_paths: Vec<Vec<u8>>,    // NUL-terminated; empty for My Computer
    place_kind: Vec<u8>,
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

    fn commit(&mut self) {
        self.items.clear();
        for i in 0..self.names.len() {
            let (icon, iw, ih) = self.icon_for(self.kinds[i]);
            self.items.push(IconItem { label: self.names[i].as_ptr(), icon, iw, ih });
        }
        Node(self.view).iconview_set(&self.items);
        self.update_sidebar();
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

    fn load_dir(&mut self, path: &[u8]) {
        let d = unsafe { nwui_dir_open(nul(path).as_ptr()) };
        if d.is_null() {
            return;
        }
        self.my_computer = false;
        self.cwd = nul(path);
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
    }

    fn nav_up(&mut self) {
        if self.my_computer {
            return;
        }
        let cwd = self.cwd[..self.cwd.len() - 1].to_vec();
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
            self.load_my_computer();
        } else {
            let p = self.place_paths[idx][..self.place_paths[idx].len() - 1].to_vec();
            self.load_dir(&p);
        }
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
        app.load_dir(&h);
    }
}
extern "C" fn cb_place(n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).go_place(n) }
}
extern "C" fn cb_mycomputer(_n: *mut NwNode, user: *mut c_void) {
    unsafe { (&mut *(user as *mut App)).load_my_computer() }
}
extern "C" fn cb_close(_n: *mut NwNode, _user: *mut c_void) {
    unsafe { libnwui_rs::exit(0) }
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
        place_nodes: Vec::new(),
        place_paths: Vec::new(),
        place_kind: Vec::new(),
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

    let toolbar = ui.hbox()
        .add(ui.link_raw("Up", cb_up, app_ptr))
        .add(ui.link_raw("Home", cb_homebtn, app_ptr))
        .add(crumb.flex(1))
        .gap(8)
        .pad(8)
        .colors(0x001d2733, 0x00eef2f8);   /* a defined toolbar strip */

    let body = ui.hbox().add(sidebar).add(view.flex(1));
    let root = ui.vbox().add(toolbar).add(body.flex(1));

    unsafe { (&mut *(app_ptr as *mut App)).load_my_computer(); }
    ui.focus(view);
    ui.run(root);
    0
}
