//! MayteraOS Rust Kernel Components
//! 
//! Memory-safe implementations of kernel subsystems.

#![no_std]
#![no_main]

pub mod ipc;
pub mod capability;
pub mod graphfs;

use core::panic::PanicInfo;

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
