//! libnwui-rs — safe Rust bindings over the C-ABI libnwui toolkit, for nanowm apps. A `no_std`
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
    fn nwui_link(u: *mut NwUi, text: *const u8, cb: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_link_set_active(n: *mut NwNode, active: i32);
    fn nwui_colors(n: *mut NwNode, fg: u32, bg: u32) -> *mut NwNode;
    fn nwui_panel(u: *mut NwUi, title: *const u8) -> *mut NwNode;
    fn nwui_iconview(u: *mut NwUi, act: RawCb, chg: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_iconview_set(n: *mut NwNode, items: *const IconItem, count: i32);
    fn nwui_iconview_selected(n: *mut NwNode) -> i32;
    fn nwui_iconview_is_selected(n: *mut NwNode, i: i32) -> i32;
    fn nwui_iconview_selection_count(n: *mut NwNode) -> i32;
    fn nwui_iconview_select_all(n: *mut NwNode);
    fn nwui_iconview_clear_selection(n: *mut NwNode);
    fn nwui_iconview_set_dnd(n: *mut NwNode, on_drag: RawCb, on_drop: RawCb);
    fn nwui_iconview_set_clipboard(n: *mut NwNode, on_copy: RawCb, on_paste: RawCb);
    fn nwui_iconview_copy_cut(n: *mut NwNode) -> i32;
    fn nwui_iconview_drop_cell(n: *mut NwNode) -> i32;
    fn nwui_iconview_drop_mods(n: *mut NwNode) -> i32;
    fn nwui_iconview_drop_text(n: *mut NwNode) -> *const u8;
    fn nwui_begin_drag(u: *mut NwUi, text: *const u8);
    fn nwui_vbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_hbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_add(parent: *mut NwNode, child: *mut NwNode) -> *mut NwNode;
    fn nwui_pad(n: *mut NwNode, p: i32) -> *mut NwNode;
    fn nwui_gap(n: *mut NwNode, g: i32) -> *mut NwNode;
    fn nwui_flex(n: *mut NwNode, f: i32) -> *mut NwNode;
    fn nwui_size(n: *mut NwNode, w: i32, h: i32) -> *mut NwNode;
    fn nwui_set_text(n: *mut NwNode, t: *const u8);
    fn nwui_spawn(u: *mut NwUi, cmd: *const u8);
    fn nwui_spawn_arg(u: *mut NwUi, cmd: *const u8, arg: *const u8);
    fn nwui_open_file(u: *mut NwUi, path: *const u8);
    fn nwui_open_file_with(u: *mut NwUi, path: *const u8, app: *const u8);
    fn nwui_assoc_lookup(ext: *const u8, out: *mut u8, cap: i32) -> i32;
    fn nwui_open_modal(u: *mut NwUi, subtree: *mut NwNode, on_close: Option<RawCb>, user: *mut c_void);
    fn nwui_close_modal(u: *mut NwUi);
    fn nwui_image_load_png(path: *const u8, w: *mut i32, h: *mut i32) -> *mut u32;
    fn nwui_menu(u: *mut NwUi, title: *const u8) -> i32;
    fn nwui_menu_item(u: *mut NwUi, menu: i32, label: *const u8, cb: RawCb, user: *mut c_void);
    fn nwui_context_clear(u: *mut NwUi);
    fn nwui_context_add(u: *mut NwUi, label: *const u8, cb: RawCb, user: *mut c_void);
    fn nwui_iconbtn(u: *mut NwUi, icon: *const u32, iw: i32, ih: i32, cb: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_textfield(u: *mut NwUi, buf: *mut u8, cap: i32, on_change: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_textfield_set(n: *mut NwNode, s: *const u8);
    fn nwui_textfield_set_submit(n: *mut NwNode, cb: RawCb);
    fn nwui_textfield_select_all(n: *mut NwNode);
    fn nwui_message(u: *mut NwUi, title: *const u8, text: *const u8);
    fn nwui_prompt(u: *mut NwUi, title: *const u8, buf: *mut u8, cap: i32, on_ok: RawCb, user: *mut c_void);
    fn nwui_confirm(u: *mut NwUi, title: *const u8, text: *const u8, ok_label: *const u8,
                    on_yes: RawCb, user: *mut c_void);
    fn nwui_accel(u: *mut NwUi, ctrl: i32, key: i8, fkey: i32, cb: RawCb, user: *mut c_void);
}

/* ---- filesystem operations (nwui_fs.c; flat ABI, no Ui handle) ---- */
extern "C" {
    fn nwui_fs_mkdir(path: *const u8) -> i32;
    fn nwui_fs_rename(from: *const u8, to: *const u8) -> i32;
    fn nwui_fs_exists(path: *const u8) -> i32;
    fn nwui_fs_isdir(path: *const u8) -> i32;
    fn nwui_fs_size(path: *const u8) -> i64;
    fn nwui_fs_remove(path: *const u8) -> i32;
    fn nwui_fs_copy(from: *const u8, to: *const u8) -> i32;
    fn nwui_fs_space(path: *const u8, avail: *mut u64, total: *mut u64) -> i32;
}

/// Thin safe-ish wrappers over the C filesystem ops. Paths are caller-supplied NUL-terminated
/// byte slices (the explorer already stores every path that way).
pub mod fs {
    pub fn mkdir(path: &[u8]) -> bool { unsafe { super::nwui_fs_mkdir(path.as_ptr()) == 0 } }
    pub fn rename(from: &[u8], to: &[u8]) -> bool { unsafe { super::nwui_fs_rename(from.as_ptr(), to.as_ptr()) == 0 } }
    pub fn exists(path: &[u8]) -> bool { unsafe { super::nwui_fs_exists(path.as_ptr()) == 1 } }
    pub fn is_dir(path: &[u8]) -> bool { unsafe { super::nwui_fs_isdir(path.as_ptr()) == 1 } }
    pub fn size(path: &[u8]) -> i64 { unsafe { super::nwui_fs_size(path.as_ptr()) } }
    pub fn remove(path: &[u8]) -> bool { unsafe { super::nwui_fs_remove(path.as_ptr()) == 0 } }
    pub fn copy(from: &[u8], to: &[u8]) -> bool { unsafe { super::nwui_fs_copy(from.as_ptr(), to.as_ptr()) == 0 } }
    /// (avail_bytes, total_bytes) for the volume holding `path`, or None on error.
    pub fn space(path: &[u8]) -> Option<(u64, u64)> {
        let (mut a, mut t) = (0u64, 0u64);
        if unsafe { super::nwui_fs_space(path.as_ptr(), &mut a, &mut t) } == 0 { Some((a, t)) } else { None }
    }
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
    pub fn colors(self, fg: u32, bg: u32) -> Node { unsafe { Node(nwui_colors(self.0, fg, bg)) } }
    pub fn set_active(self, on: bool) { unsafe { nwui_link_set_active(self.0, on as i32) } }
    /// Set the iconview's items. The slice must outlive the view (libnwui keeps the pointer).
    pub fn iconview_set(self, items: &[IconItem]) {
        unsafe { nwui_iconview_set(self.0, items.as_ptr(), items.len() as i32) }
    }
    pub fn iconview_selected(self) -> i32 { unsafe { nwui_iconview_selected(self.0) } }
    /// Whole multi-selection (Shift/Cmd-click): test a cell, count, select-all, clear.
    pub fn iconview_is_selected(self, i: i32) -> bool { unsafe { nwui_iconview_is_selected(self.0, i) != 0 } }
    pub fn iconview_selection_count(self) -> i32 { unsafe { nwui_iconview_selection_count(self.0) } }
    pub fn iconview_select_all(self) { unsafe { nwui_iconview_select_all(self.0) } }
    pub fn iconview_clear_selection(self) { unsafe { nwui_iconview_clear_selection(self.0) } }
    /// Set a textfield's displayed value (NUL-terminated ptr); does not fire on_change.
    pub fn textfield_set(self, s: *const u8) { unsafe { nwui_textfield_set(self.0, s) } }
    /// Fire `cb` when Enter is pressed in this textfield (submit, vs per-keystroke on_change).
    pub fn textfield_set_submit(self, cb: RawCb) { unsafe { nwui_textfield_set_submit(self.0, cb) } }
    /// Select the whole field (next keystroke replaces it).
    pub fn textfield_select_all(self) { unsafe { nwui_textfield_select_all(self.0) } }
    /// Make this iconview a drag source (on_drag) + drop target (on_drop), raw-callback ABI.
    pub fn iconview_set_dnd(self, on_drag: RawCb, on_drop: RawCb) {
        unsafe { nwui_iconview_set_dnd(self.0, on_drag, on_drop) }
    }
    /// Make this iconview handle Cmd+C/X (on_copy) + Cmd+V (on_paste) — file clipboard ops.
    pub fn iconview_set_clipboard(self, on_copy: RawCb, on_paste: RawCb) {
        unsafe { nwui_iconview_set_clipboard(self.0, on_copy, on_paste) }
    }
    /// During on_copy: 1 if it was a cut (Cmd+X), 0 if a copy (Cmd+C).
    pub fn iconview_copy_cut(self) -> i32 { unsafe { nwui_iconview_copy_cut(self.0) } }
    /// During on_drop: the cell index the drop landed on, or -1 for the empty area.
    pub fn iconview_drop_cell(self) -> i32 { unsafe { nwui_iconview_drop_cell(self.0) } }
    /// During on_drop: modifier bits at the drop (bit1 = Ctrl held -> copy instead of move).
    pub fn iconview_drop_mods(self) -> i32 { unsafe { nwui_iconview_drop_mods(self.0) } }
    /// During on_drop: the dropped payload as a raw NUL-terminated C pointer (or null).
    pub fn iconview_drop_text(self) -> *const u8 { unsafe { nwui_iconview_drop_text(self.0) } }
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

    /// A flat sidebar link/nav-row (raw callback ABI). Mark the current one with Node::set_active.
    pub fn link_raw(&self, s: &str, cb: RawCb, user: *mut c_void) -> Node {
        let c = cstr(s);
        unsafe { Node(nwui_link(self.0, c.as_ptr(), cb, user)) }
    }

    /// A flat clickable toolbar icon button. `icon` = (pixels, w, h) from load_png.
    pub fn iconbtn(&self, icon: (*const u32, i32, i32), cb: RawCb, user: *mut c_void) -> Node {
        unsafe { Node(nwui_iconbtn(self.0, icon.0, icon.1, icon.2, cb, user)) }
    }

    /// An editable text field over an app-owned buffer; on_change fires (raw ABI) as text changes.
    pub fn textfield(&self, buf: *mut u8, cap: i32, on_change: RawCb, user: *mut c_void) -> Node {
        unsafe { Node(nwui_textfield(self.0, buf, cap, on_change, user)) }
    }

    /// An icon-grid view using the raw C callback ABI: `on_activate` (double-click/Enter) and
    /// `on_change` (selection changed) both receive the same `user` pointer.
    pub fn iconview_raw(&self, on_activate: RawCb, on_change: RawCb, user: *mut c_void) -> Node {
        unsafe { Node(nwui_iconview(self.0, on_activate, on_change, user)) }
    }

    /* Global menu: declare a top-bar menu (returns its index) + add items (raw callback ABI).
     * The compositor renders the focused window's menus in the system menu bar. */
    pub fn menu(&self, title: &str) -> i32 { let c = cstr(title); unsafe { nwui_menu(self.0, c.as_ptr()) } }
    pub fn menu_item(&self, menu: i32, label: &str, cb: RawCb, user: *mut c_void) {
        let c = cstr(label);
        unsafe { nwui_menu_item(self.0, menu, c.as_ptr(), cb, user) }
    }

    /* Right-click context menu: clear, then add items (raw callback ABI). It pops up
     * automatically when an iconview is right-clicked. Labels must outlive the Ui (libnwui
     * keeps the pointer), so pass &'static str literals. */
    pub fn context_clear(&self) { unsafe { nwui_context_clear(self.0) } }
    pub fn context_add(&self, label: &'static str, cb: RawCb, user: *mut c_void) {
        // NUL-terminate; leak it (the menu holds the pointer for the app's lifetime).
        let mut v = alloc::vec::Vec::with_capacity(label.len() + 1);
        v.extend_from_slice(label.as_bytes());
        v.push(0);
        let p = v.as_ptr();
        core::mem::forget(v);
        unsafe { nwui_context_add(self.0, p, cb, user) }
    }
    /// An info/alert dialog (title + one line + OK).
    pub fn message(&self, title: &str, text: &str) {
        let (t, x) = (cstr(title), cstr(text));
        unsafe { nwui_message(self.0, t.as_ptr(), x.as_ptr()) }
    }

    /// A text-input dialog over an app-owned buffer (stable address required); OK fires on_ok.
    pub fn prompt(&self, title: &str, buf: *mut u8, cap: i32, on_ok: RawCb, user: *mut c_void) {
        let t = cstr(title);
        unsafe { nwui_prompt(self.0, t.as_ptr(), buf, cap, on_ok, user) }
    }

    /// A confirmation dialog; the affirmative button reads `ok_label` and fires on_yes.
    pub fn confirm(&self, title: &str, text: &str, ok_label: &str, on_yes: RawCb, user: *mut c_void) {
        let (t, x, k) = (cstr(title), cstr(text), cstr(ok_label));
        unsafe { nwui_confirm(self.0, t.as_ptr(), x.as_ptr(), k.as_ptr(), on_yes, user) }
    }

    /// Start a drag carrying `text` (NUL-terminated) as the payload; call from an on_drag handler.
    pub fn begin_drag(&self, text: *const u8) { unsafe { nwui_begin_drag(self.0, text) } }

    /// Register a keyboard accelerator: Ctrl+<key> (ctrl=true, key=b'c', fkey=0) or a function/
    /// special key (ctrl=false, key=0, fkey=<scancode>). Fires cb and consumes the keystroke.
    pub fn accel(&self, ctrl: bool, key: u8, fkey: i32, cb: RawCb, user: *mut c_void) {
        unsafe { nwui_accel(self.0, ctrl as i32, key as i8, fkey, cb, user) }
    }

    pub fn focus(&self, n: Node) { unsafe { nwui_focus(self.0, n.0) } }
    pub fn spawn(&self, cmd: &str) { let c = cstr(cmd); unsafe { nwui_spawn(self.0, c.as_ptr()) } }
    /// macOS-style "open": launch a file (NUL-terminated ptr) in its associated app.
    pub fn open_file(&self, path: *const u8) { unsafe { nwui_open_file(self.0, path) } }
    /// Open a file with a SPECIFIC app (a per-file default-program override).
    pub fn open_file_with(&self, path: *const u8, app: &str) {
        let c = cstr(app);
        unsafe { nwui_open_file_with(self.0, path, c.as_ptr()) }
    }
    /// Resolve an extension (lowercase, no dot) to its associated app name, or None.
    pub fn assoc_lookup(&self, ext: &[u8]) -> Option<alloc::vec::Vec<u8>> {
        let mut e = alloc::vec::Vec::with_capacity(ext.len() + 1);
        e.extend_from_slice(ext); e.push(0);
        let mut out = [0u8; 64];
        let ok = unsafe { nwui_assoc_lookup(e.as_ptr(), out.as_mut_ptr(), 64) };
        if ok == 0 { return None; }
        let mut n = 0; while n < out.len() && out[n] != 0 { n += 1; }
        Some(out[..n].to_vec())
    }
    /// Open `subtree` as a modal overlay (blocks the window until close_modal).
    pub fn open_modal(&self, subtree: Node) {
        unsafe { nwui_open_modal(self.0, subtree.0, None, core::ptr::null_mut()) }
    }
    pub fn close_modal(&self) { unsafe { nwui_close_modal(self.0) } }
    /// Launch `cmd` with `arg` (a NUL-terminated C pointer) as its argv[1] — "open with".
    pub fn spawn_arg(&self, cmd: &str, arg: *const u8) {
        let c = cstr(cmd);
        unsafe { nwui_spawn_arg(self.0, c.as_ptr(), arg) }
    }
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
