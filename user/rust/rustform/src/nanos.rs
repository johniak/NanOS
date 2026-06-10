//! nanos — a tiny Rust SDK for NanWM apps. A `no_std` runtime (allocator over the C malloc,
//! panic = abort) plus safe, idiomatic wrappers over the C `libnwui` toolkit: buttons take
//! Rust CLOSURES, containers compose with method chaining. The window server, wire protocol
//! and rendering are all the same C stack — only the app language changes (this is what a
//! stable C ABI buys, exactly like a Rust app binding to user32).
//!
//! This is a module of the app crate (crate-level attributes live in the app's root file).

use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::panic::PanicInfo;
use alloc::boxed::Box;
use alloc::vec::Vec;

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn exit(code: i32) -> !;
    fn write(fd: i32, p: *const u8, n: usize) -> isize;
}

/* ---- runtime: heap over the C library + abort-on-panic ---- */
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
type RawCb = extern "C" fn(*mut NwNode, *mut c_void);

extern "C" {
    fn nwui_open(title: *const u8, w: i32, h: i32) -> *mut NwUi;
    fn nwui_set_root(u: *mut NwUi, root: *mut NwNode);
    fn nwui_run(u: *mut NwUi);
    fn nwui_label(u: *mut NwUi, text: *const u8) -> *mut NwNode;
    fn nwui_button(u: *mut NwUi, text: *const u8, cb: RawCb, user: *mut c_void) -> *mut NwNode;
    fn nwui_textfield(u: *mut NwUi, buf: *mut u8, cap: i32, cb: *const c_void, user: *mut c_void) -> *mut NwNode;
    fn nwui_vbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_hbox(u: *mut NwUi) -> *mut NwNode;
    fn nwui_add(parent: *mut NwNode, child: *mut NwNode) -> *mut NwNode;
    fn nwui_pad(n: *mut NwNode, p: i32) -> *mut NwNode;
    fn nwui_gap(n: *mut NwNode, g: i32) -> *mut NwNode;
    fn nwui_flex(n: *mut NwNode, f: i32) -> *mut NwNode;
    fn nwui_set_text(n: *mut NwNode, t: *const u8);
}

/* NUL-terminate a &str for a C call. The C side copies captions immediately, so the buffer
 * only needs to outlive the call. */
fn cstr(s: &str) -> Vec<u8> {
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
    pub fn add(self, c: Node) -> Node { unsafe { nwui_add(self.0, c.0); } self }
    pub fn set_text(self, s: &str) { let c = cstr(s); unsafe { nwui_set_text(self.0, c.as_ptr()) } }
}

/* The closure trampoline: libnwui calls this C function with the boxed closure as `user`. */
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
    pub fn vbox(&self) -> Node { unsafe { Node(nwui_vbox(self.0)) } }
    pub fn hbox(&self) -> Node { unsafe { Node(nwui_hbox(self.0)) } }
    pub fn textfield(&self, buf: &'static mut [u8]) -> Node {
        unsafe {
            Node(nwui_textfield(self.0, buf.as_mut_ptr(), buf.len() as i32,
                                core::ptr::null(), core::ptr::null_mut()))
        }
    }
    /// A button whose click runs a Rust closure (boxed + leaked for the app's lifetime).
    pub fn button<F: FnMut() + 'static>(&self, s: &str, f: F) -> Node {
        let boxed: Box<dyn FnMut()> = Box::new(f);
        let user = Box::into_raw(Box::new(boxed)) as *mut c_void;
        let c = cstr(s);
        unsafe { Node(nwui_button(self.0, c.as_ptr(), trampoline, user)) }
    }
    pub fn run(&self, root: Node) { unsafe { nwui_set_root(self.0, root.0); nwui_run(self.0) } }
}
