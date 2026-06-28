//! rustform — the greeting form (cf. the C form), written in Rust against libnwui through
//! the `nanos` SDK. Proves a Rust app runs on NanOS using the same window server, protocol
//! and shared toolkit as the C apps — only the language differs. Buttons use Rust closures.
#![no_std]

extern crate alloc;

mod nanos;
use nanos::{Node, Ui};
use alloc::format;
use alloc::string::String;

/// Read a NUL-terminated C string (the textfield buffer libnwui writes into) as a String.
fn read_cstr(p: *const u8) -> String {
    let mut s = String::new();
    unsafe {
        let mut i = 0isize;
        loop {
            let b = *p.offset(i);
            if b == 0 {
                break;
            }
            s.push(b as char);
            i += 1;
        }
    }
    s
}

/// Entry point: our crt0 calls `main` (C ABI), exactly like a C app.
#[no_mangle]
pub extern "C" fn main() -> i32 {
    let ui = match Ui::open("Greeter (Rust)", 360, 200) {
        Some(u) => u,
        None => return 1,
    };

    // The textfield's value lives in a leaked buffer (stable for the app's lifetime); libnwui
    // writes the typed text into it.
    let name: &'static mut [u8] = alloc::vec![0u8; 64].leak();
    let name_ptr = name.as_ptr();
    let field: Node = ui.textfield(name);
    let result: Node = ui.label("");

    let greet = ui.button("Greet", move || {
        let n = read_cstr(name_ptr);
        let msg = if n.is_empty() { String::from("Hello there!") } else { format!("Hello, {}!", n) };
        result.set_text(&msg);
    });
    let clear = ui.button("Clear", move || {
        field.set_text("");
        result.set_text("");
    });

    let root = ui
        .vbox()
        .pad(12)
        .gap(8)
        .add(ui.label("Your name:"))
        .add(field.flex(0))
        .add(ui.hbox().gap(8).add(greet).add(clear))
        .add(result);

    ui.run(root);
    0
}
