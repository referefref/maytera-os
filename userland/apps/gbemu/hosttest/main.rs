// hosttest/main.rs - run the gbemu core on an ordinary Linux host, in seconds,
// with no VM and no compositor. Same pattern as userland/lib/opl2/hosttest and
// userland/lib/midi/hosttest already established in this tree: the SAME
// source (gb.rs) compiles unchanged for the host (std) and for the Ring-3 ELF
// (no_std + a libc-malloc-backed GlobalAlloc), via `extern crate alloc`, which
// works identically under both (verified: a plain `rustc` std build using
// `extern crate alloc; use alloc::vec::Vec;` compiles and runs with no special
// flags). That makes a host run real evidence about the shipped core, not
// about a lookalike.
//
// It does NOT replace the in-OS run. The host arm proves CPU/PPU/MBC/timer
// correctness against test ROMs quickly; only the in-OS arm proves the Ring-3
// ELF links, that its window/event-queue/input plumbing works, and that nDS
// (compositor) blit succeeds. Both are reported separately.
//
// Usage:
//   rustc --edition 2021 -O -o /tmp/gbhosttest hosttest/main.rs
//   /tmp/gbhosttest <rom.gb> [--frames N] [--dump-ppm out.ppm] [--serial]
//
// Exit status: for a Blargg-style test ROM, 0 if "Passed" appears in the
// captured serial stream, 1 if "Failed" appears, 2 if neither appeared within
// the frame budget (inconclusive - the ROM did not finish signalling).

#[path = "../gb.rs"]
mod gb;

use std::env;
use std::fs;
use std::io::Write;

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("usage: {} <rom.gb> [--frames N] [--dump-ppm out.ppm] [--serial]", args[0]);
        std::process::exit(2);
    }
    let rom_path = &args[1];
    let mut frames: u32 = 3000; // ~50s of emulated time at 59.7fps, plenty for cpu_instrs
    let mut dump_ppm: Option<String> = None;
    let mut show_serial = false;
    // Scripted input for iterating past a title/menu screen without the OS:
    // "--press 100:START,140:A,300:A" means "hold START down at frame 100 for
    // one frame, A at 140, A at 300". Each entry is press-then-release within
    // the same frame (long enough for a poll loop to see it).
    let mut script: Vec<(u32, gb::Button)> = Vec::new();
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "--frames" => {
                i += 1;
                frames = args[i].parse().unwrap_or(frames);
            }
            "--dump-ppm" => {
                i += 1;
                dump_ppm = Some(args[i].clone());
            }
            "--serial" => show_serial = true,
            "--press" => {
                i += 1;
                for tok in args[i].split(',') {
                    let mut parts = tok.splitn(2, ':');
                    let f: u32 = parts.next().unwrap_or("0").parse().unwrap_or(0);
                    let b = match parts.next().unwrap_or("") {
                        "A" => gb::Button::A,
                        "B" => gb::Button::B,
                        "START" => gb::Button::Start,
                        "SELECT" => gb::Button::Select,
                        "UP" => gb::Button::Up,
                        "DOWN" => gb::Button::Down,
                        "LEFT" => gb::Button::Left,
                        "RIGHT" => gb::Button::Right,
                        _ => continue,
                    };
                    script.push((f, b));
                }
            }
            _ => {}
        }
        i += 1;
    }

    let rom = fs::read(rom_path).unwrap_or_else(|e| {
        eprintln!("failed to read {}: {}", rom_path, e);
        std::process::exit(2);
    });
    println!(
        "gbemu hosttest: {} ({} bytes), running up to {} frames",
        rom_path,
        rom.len(),
        frames
    );
    let mut g = gb::Gb::new(rom);
    {
        let t = g.title();
        let s: String = t.iter().take_while(|&&b| b != 0).map(|&b| b as char).collect();
        println!("cart title: {:?}  cart_type=0x{:02X}  rom_banks={}",
            s, g.cart.cart_type, g.cart.num_rom_banks);
        println!("mbc kind: {}", match g.cart.kind {
            gb::MbcKind::None => "None",
            gb::MbcKind::Mbc1 => "MBC1",
            gb::MbcKind::Mbc3 => "MBC3",
            gb::MbcKind::Mbc5 => "MBC5",
            gb::MbcKind::Unknown => "Unknown",
        });
    }

    let mut last_len = 0usize;
    let mut verdict = 2i32;
    for f in 0..frames {
        for &(sf, btn) in script.iter() {
            if sf == f {
                g.joyp.set(btn, true);
            }
            if sf.wrapping_add(2) == f {
                g.joyp.set(btn, false);
            }
        }
        g.run_frame();
        if g.serial_log.len() > last_len {
            if show_serial {
                let new_bytes = &g.serial_log[last_len..];
                print!("{}", String::from_utf8_lossy(new_bytes));
                std::io::stdout().flush().ok();
            }
            last_len = g.serial_log.len();
            let text = String::from_utf8_lossy(&g.serial_log);
            if text.contains("Failed") {
                verdict = 1;
                println!("\n[frame {}] FAILED detected in serial output", f);
                break;
            }
            if text.contains("Passed") {
                verdict = 0;
                println!("\n[frame {}] PASSED detected in serial output", f);
                break;
            }
        }
    }

    if verdict == 2 {
        println!(
            "inconclusive: no Passed/Failed marker seen in {} frames ({} serial bytes captured)",
            frames,
            g.serial_log.len()
        );
        if !g.serial_log.is_empty() {
            println!("--- captured serial so far ---");
            println!("{}", String::from_utf8_lossy(&g.serial_log));
        }
    }

    if let Some(path) = dump_ppm {
        let fb = &g.ppu.fb;
        let mut out = String::new();
        out.push_str(&format!("P6\n{} {}\n255\n", gb::SCREEN_W, gb::SCREEN_H));
        let mut bytes = out.into_bytes();
        for &px in fb.iter() {
            bytes.push(((px >> 16) & 0xFF) as u8);
            bytes.push(((px >> 8) & 0xFF) as u8);
            bytes.push((px & 0xFF) as u8);
        }
        fs::write(&path, &bytes).unwrap();
        println!("wrote framebuffer to {}", path);
    }

    println!("total M-cycles executed: {}", g.total_m_cycles);
    std::process::exit(verdict);
}
