#[path = "../gb.rs"]
mod gb;
use std::fs;
fn main() {
    let rom = fs::read(std::env::args().nth(1).unwrap()).unwrap();
    let mut g = gb::Gb::new(rom);
    let frames: u32 = std::env::args().nth(2).and_then(|s| s.parse().ok()).unwrap_or(1200);
    let mut total = 0usize;
    let mut nonzero = 0usize;
    let mut peak: i16 = 0;
    for _ in 0..frames {
        g.run_frame();
        for &s in g.apu.out.iter() {
            total += 1;
            if s != 0 { nonzero += 1; }
            if s.abs() > peak { peak = s.abs(); }
        }
        g.apu.out.clear();
    }
    println!("frames={} samples={} nonzero={} ({}%) peak={}",
        frames, total, nonzero, if total>0 {nonzero*100/total} else {0}, peak);
    println!("=> {}", if nonzero > 0 { "APU IS PRODUCING AUDIO" } else { "SILENT (no non-zero samples)" });
}
