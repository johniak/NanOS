//! libnwui-rs — safe Rust bindings over the C-ABI libnwui toolkit, for NanWM apps. A `no_std`
//! crate: the runtime (heap over the C malloc, panic = abort) plus idiomatic wrappers over
//! windows, the icon-grid (`iconview`) and titled-panel (`panel`) widgets, image loading, menus
//! and spawn. The window server, wire protocol and rendering are all the same C stack — only the
//! app language changes (what a stable C ABI buys, exactly like a Rust app binding to user32).
#![no_std]

extern crate alloc;

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::panic::PanicInfo;
use alloc::boxed::Box;
use alloc::vec::Vec;

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    pub fn exit(code: i32) -> !;
    fn write(fd: i32, p: *const u8, n: usize) -> isize;
}

/* ---- runtime: heap over the C library + abort-on-panic (one definition for the whole app) ---- */
struct Libc;
unsafe impl GlobalAlloc for Libc {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { malloc(l.size().max(1)) }
    unsafe fn dealloc(&self, p: *mut u8, _l: Layout) { free(p) }
}
#[global_allocator]
static ALLOC: Libc = Libc;

#[panic_handler]
fn on_panic(_: &PanicInfo) -> ! {
    let m = b"rust: panic\n";
    unsafe { write(2, m.as_ptr(), m.len()); exit(101) }
}

/* ---- raw FFI to libnwui (plain C ABI) ---- */
#[repr(C)] pub struct NwUi { _o: [u8; 0] }
#[repr(C)] pub struct NwNode { _o: [u8; 0] }

/// The C callback ABI: `void (*)(nwui_node *self, void *user)`. Apps that need the `user` pointer
/// (e.g. shared explorer state) implement this directly and pass it to the `_raw` constructors.
pub type RawCb = extern "C" fn(*mut NwNode, *mut c_void);

/// Mirrors the C `nwui_icon_item { const char *label; const uint32_t *icon; int iw, ih; }`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct IconItem {
    pub label: *const u8,
    pub icon: *const u32,
    pub iw: i32,
    pub ih: i32,
}

extern "C" {
    fn nwui_open(title: *const u8, w: i32, h: i32) -> *mut NwUi;
    fn nwui_set_root(u: *mut NwUi, root: *mut NwNode);
    fn nwui_focus(u: *mut NwUi, n: *mut NwNode);
    fn nwui_run(u: *mut NwUi);
    fn nwui_label(u: *mut NwUi, text: *const u8) -> *mut NwNode;
    fn nwui_button(u: *mut NwUi, text: *const u8, cb: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_panel(u: *mut NwUi, title: *const u8) -> *mut NwNode;
    fn nwui_iconview(u: *mut NwUi, act: RawCb, chg: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_iconview_set(n: *mut NwNode, items: *const IconItem, count: i32);
    fn nwui_iconview_selected(n: *mut NwNode) -> i32;
    fn nwui_vbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_hbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_add(parent: *mut NwNode, child: *mut NwNode) -> *mut NwNode;
    fn nwui_pad(n: *mut NwNode, p: i32) -> *mut NwNode;
    fn nwui_gap(n: *mut NwNode, g: i32) -> *mut NwNode;
    fn nwui_flex(n: *mut NwNode, f: i32) -> *mut NwNode;
    fn nwui_size(n: *mut NwNode, w: i32, h: i32) -> *mut NwNode;
    fn nwui_set_text(n: *mut NwNode, t: *const u8);
    fn nwui_spawn(u: *mut NwUi, cmd: *const u8);
    fn nwui_image_load_png(path: *const u8, w: *mut i32, h: *mut i32) -> *mut u32;
}

/// NUL-terminate a `&str` for a C call. libnwui copies captions immediately, so the buffer only
/// needs to outlive the call.
pub fn cstr(s: &str) -> Vec<u8> {
    let mut v = Vec::with_capacity(s.len() + 1);
    v.extend_from_slice(s.as_bytes());
    v.push(0);
    v
}

/* ---- safe wrappers ---- */
#[derive(Clone, Copy)]
pub struct Node(pub *mut NwNode);
impl Node {
    pub fn pad(self, p: i32) -> Node { unsafe { Node(nwui_pad(self.0, p)) } }
    pub fn gap(self, g: i32) -> Node { unsafe { Node(nwui_gap(self.0, g)) } }
    pub fn flex(self, f: i32) -> Node { unsafe { Node(nwui_flex(self.0, f)) } }
    pub fn size(self, w: i32, h: i32) -> Node { unsafe { Node(nwui_size(self.0, w, h)) } }
    pub fn add(self, c: Node) -> Node { unsafe { nwui_add(self.0, c.0); } self }
    pub fn set_text(self, s: &str) { let c = cstr(s); unsafe { nwui_set_text(self.0, c.as_ptr()) } }
    /// Set the iconview's items. The slice must outlive the view (libnwui keeps the pointer).
    pub fn iconview_set(self, items: &[IconItem]) {
        unsafe { nwui_iconview_set(self.0, items.as_ptr(), items.len() as i32) }
    }
    pub fn iconview_selected(self) -> i32 { unsafe { nwui_iconview_selected(self.0) } }
}

/// The closure trampoline: libnwui calls this C function with the boxed closure as `user`.
extern "C" fn trampoline(_self: *mut NwNode, user: *mut c_void) {
    unsafe { let f = &mut *(user as *mut Box<dyn FnMut()>); f() }
}

pub struct Ui(pub *mut NwUi);
impl Ui {
    pub fn open(title: &str, w: i32, h: i32) -> Option<Ui> {
        let t = cstr(title);
        let p = unsafe { nwui_open(t.as_ptr(), w, h) };
        if p.is_null() { None } else { Some(Ui(p)) }
    }
    pub fn label(&self, s: &str) -> Node { let c = cstr(s); unsafe { Node(nwui_label(self.0, c.as_ptr())) } }
    pub fn panel(&self, title: &str) -> Node { let c = cstr(title); unsafe { Node(nwui_panel(self.0, c.as_ptr())) } }
    pub fn vbox(&self) -> Node { unsafe { Node(nwui_vbox(self.0)) } }
    pub fn hbox(&self) -> Node { unsafe { Node(nwui_hbox(self.0)) } }

    /// A button whose click runs a Rust closure (boxed + leaked for the app's lifetime).
    pub fn button<F: FnMut() + 'static>(&self, s: &str, f: F) -> Node {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let c = cstr(s);
        unsafe { Node(nwui_button(self.0, c.as_ptr(), trampoline, user)) }
    }

    /// A button using the raw C callback ABI + a `user` pointer — for callbacks that need shared
    /// app state (the explorer threads its `*mut App` through this).
    pub fn button_raw(&self, s: &str, cb: RawCb, user: *mut c_void) -> Node {
        let c = cstr(s);
        unsafe { Node(nwui_button(self.0, c.as_ptr(), cb, user)) }
    }

    /// An icon-grid view using the raw C callback ABI: `on_activate` (double-click/Enter) and
    /// `on_change` (selection changed) both receive the same `user` pointer.
    pub fn iconview_raw(&self, on_activate: RawCb, on_change: RawCb, user: *mut c_void) -> Node {
        unsafe { Node(nwui_iconview(self.0, on_activate, on_change, user)) }
    }

    pub fn focus(&self, n: Node) { unsafe { nwui_focus(self.0, n.0) } }
    pub fn spawn(&self, cmd: &str) { let c = cstr(cmd); unsafe { nwui_spawn(self.0, c.as_ptr()) } }
    pub fn run(&self, root: Node) { unsafe { nwui_set_root(self.0, root.0); nwui_run(self.0) } }
}

/// Load a PNG file via the toolkit decoder. Returns (pixels, w, h); the buffer is malloc'd by C
/// and intentionally leaked (icons live for the whole app). None on any failure.
pub fn load_png(path: &str) -> Option<(*const u32, i32, i32)> {
    let c = cstr(path);
    let mut w = 0i32;
    let mut h = 0i32;
    let p = unsafe { nwui_image_load_png(c.as_ptr(), &mut w, &mut h) };
    if p.is_null() { None } else { Some((p as *const u32, w, h)) }
}
