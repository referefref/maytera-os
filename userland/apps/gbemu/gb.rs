// gb.rs - MayteraOS Game Boy (DMG / SM83-LR35902) emulator core.
//
// CLEAN-ROOM NOTICE. This file implements the SM83/LR35902 CPU (base +
// CB-prefixed instruction set, flags, interrupts, HALT/STOP quirks), the DMG
// memory map, the MBC1/MBC3/MBC5 cartridge mappers, the PPU (background, window,
// sprites, LCD mode timing), the DIV/TIMA timer and the joypad/serial
// registers, built ENTIRELY from the public Game Boy hardware specification:
// the Pan Docs (gbdev.io/pandocs) description of the register map, memory map,
// PPU mode timing and MBC banking behaviour, and the public community-
// maintained SM83 opcode/cycle-count table (gbdev.io/gb-opcodes, the same
// shape of "standard opcode reference matrix" cited in the task brief), which
// was fetched and used only to cross-check the cycle-count constants below
// against the published hardware timing - it contains no emulator source, only
// a data table describing what the silicon does, which is not copyrightable
// expression.
//
// This project owes NO code and NO structure to any specific existing Game Boy
// emulator, including (explicitly, since the owner named it as motivation)
// https://github.com/GaelCathelin/Game-Boy-DOS. That repository carries a bare
// copyright notice with no license, i.e. all rights reserved, so it was never
// fetched, opened or read at any point during this work. See CHANGELOG.md.
//
// NO BOOT ROM. The real DMG boot ROM is Nintendo firmware; we never had it and
// never wanted it. Like effectively every legal Game Boy emulator, this core
// skips it entirely (no Nintendo logo scroll) and instead initialises CPU
// registers and I/O registers directly to the well-documented POST-BOOT-ROM
// state (Pan Docs "Power Up Sequence"), then starts execution at the
// cartridge's own entry point, 0x0100. That is a public fact about what the
// boot ROM leaves behind, not the boot ROM's code.
//
// SCOPE / HONESTY. This core uses two deliberate, documented simplifications
// relative to real silicon, both standard in the "instruction-stepped" class
// of emulator (as opposed to "T-cycle-stepped"):
//   1. Cycle accounting happens once per CPU instruction (using the public
//      per-opcode M-cycle table below, with the branch-taken/not-taken split
//      applied), not once per individual memory access within an instruction.
//      This is enough to pass cpu_instrs (which is about CPU/ALU/flag/
//      interrupt correctness) but will NOT pass Blargg's separate
//      instr_timing/mem_timing suites, which specifically probe intra-
//      instruction bus timing. Not attempted here; reported as a known gap.
//   2. The PPU renders each scanline in one shot when leaving mode 3, rather
//      than pixel-by-pixel through a FIFO. Correct for normal background/
//      window/sprite composition (including mid-frame register changes at
//      scanline granularity, e.g. per-line SCX/SCY raster effects), not
//      correct for sub-scanline (mid-line) raster tricks.
// Both are noted again at the relevant code below.

#![allow(dead_code)]
extern crate alloc;
use alloc::vec::Vec;

// ============================================================================
// Flags
// ============================================================================
pub const FLAG_Z: u8 = 0x80;
pub const FLAG_N: u8 = 0x40;
pub const FLAG_H: u8 = 0x20;
pub const FLAG_C: u8 = 0x10;

#[derive(Clone, Copy, Default)]
pub struct Regs {
    pub a: u8,
    pub f: u8,
    pub b: u8,
    pub c: u8,
    pub d: u8,
    pub e: u8,
    pub h: u8,
    pub l: u8,
    pub sp: u16,
    pub pc: u16,
}

impl Regs {
    #[inline]
    pub fn bc(&self) -> u16 {
        ((self.b as u16) << 8) | self.c as u16
    }
    #[inline]
    pub fn de(&self) -> u16 {
        ((self.d as u16) << 8) | self.e as u16
    }
    #[inline]
    pub fn hl(&self) -> u16 {
        ((self.h as u16) << 8) | self.l as u16
    }
    #[inline]
    pub fn af(&self) -> u16 {
        ((self.a as u16) << 8) | self.f as u16
    }
    #[inline]
    pub fn set_bc(&mut self, v: u16) {
        self.b = (v >> 8) as u8;
        self.c = v as u8;
    }
    #[inline]
    pub fn set_de(&mut self, v: u16) {
        self.d = (v >> 8) as u8;
        self.e = v as u8;
    }
    #[inline]
    pub fn set_hl(&mut self, v: u16) {
        self.h = (v >> 8) as u8;
        self.l = v as u8;
    }
    #[inline]
    pub fn set_af(&mut self, v: u16) {
        self.a = (v >> 8) as u8;
        self.f = (v as u8) & 0xF0; // low nibble of F is always 0 on real hardware
    }
    #[inline]
    pub fn flag(&self, mask: u8) -> bool {
        self.f & mask != 0
    }
    #[inline]
    pub fn set_flag(&mut self, mask: u8, v: bool) {
        if v {
            self.f |= mask;
        } else {
            self.f &= !mask;
        }
        self.f &= 0xF0;
    }
}

// ============================================================================
// Per-opcode M-cycle tables (public timing data; see file header).
// (base_m_cycles, extra_m_cycles_if_branch_taken)
// ============================================================================
pub const CYCLES_UNPREFIXED: [(u8, u8); 256] = [
    (1,0),(3,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),(5,0),(2,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(3,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),(3,0),(2,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),
    (2,1),(3,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),(2,1),(2,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),
    (2,1),(3,0),(2,0),(2,0),(3,0),(3,0),(3,0),(1,0),(2,1),(2,0),(2,0),(2,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (2,0),(2,0),(2,0),(2,0),(2,0),(2,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(1,0),(2,0),(1,0),
    (2,3),(3,0),(3,1),(4,0),(3,3),(4,0),(2,0),(4,0),(2,3),(4,0),(3,1),(1,0),(3,3),(6,0),(2,0),(4,0),
    (2,3),(3,0),(3,1),(0,0),(3,3),(4,0),(2,0),(4,0),(2,3),(4,0),(3,1),(0,0),(3,3),(0,0),(2,0),(4,0),
    (3,0),(3,0),(2,0),(0,0),(0,0),(4,0),(2,0),(4,0),(4,0),(1,0),(4,0),(0,0),(0,0),(0,0),(2,0),(4,0),
    (3,0),(3,0),(2,0),(1,0),(0,0),(4,0),(2,0),(4,0),(3,0),(2,0),(4,0),(1,0),(0,0),(0,0),(2,0),(4,0),
];
pub const CYCLES_CB: [u8; 256] = [
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,3,2, 2,2,2,2,2,2,3,2,
    2,2,2,2,2,2,3,2, 2,2,2,2,2,2,3,2,
    2,2,2,2,2,2,3,2, 2,2,2,2,2,2,3,2,
    2,2,2,2,2,2,3,2, 2,2,2,2,2,2,3,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
    2,2,2,2,2,2,4,2, 2,2,2,2,2,2,4,2,
];
// Illegal (removed-from-Z80) opcodes on the SM83. Executing one on real
// hardware locks the CPU permanently; we mirror that (a stuck flag) rather
// than pretending it is a NOP, since a correct program never hits one.
const ILLEGAL: [u8; 11] = [0xD3, 0xDB, 0xDD, 0xE3, 0xE4, 0xEB, 0xEC, 0xED, 0xF4, 0xFC, 0xFD];

fn is_illegal(op: u8) -> bool {
    let mut i = 0;
    while i < ILLEGAL.len() {
        if ILLEGAL[i] == op {
            return true;
        }
        i += 1;
    }
    false
}

// ============================================================================
// Cartridge / MBC
// ============================================================================
#[derive(Clone, Copy, PartialEq)]
pub enum MbcKind {
    None,
    Mbc1,
    Mbc3,
    Mbc5,
    Unknown, // recognised header we don't implement; ROM banking best-effort
}

pub struct Cart {
    pub rom: Vec<u8>,
    pub ram: Vec<u8>,
    pub kind: MbcKind,
    pub rom_bank_lo: u8, // MBC1: 5 bits. MBC3: 7 bits. MBC5: full 8 low bits.
    pub rom_bank_hi9: u8, // MBC5 only: bit 8 of the 9-bit ROM bank number (0/1)
    pub bank_hi: u8,     // MBC1: 2 bits (rom bits 5-6, or RAM bank in mode 1). MBC3/MBC5: RAM bank (MBC3 RTC select 0-3 or 8-C)
    pub ram_enabled: bool,
    pub mbc1_mode: u8, // 0 = ROM banking mode, 1 = RAM banking mode
    pub has_battery: bool,
    pub has_rtc: bool,
    // MBC3 RTC: seconds, minutes, hours, day-low, day-high(bit0=day hi,bit6=halt,bit7=carry)
    pub rtc: [u8; 5],
    pub rtc_latched: [u8; 5],
    pub rtc_latch_prev: u8,
    pub rtc_sub_cycles: u32,
    pub num_rom_banks: u32,
    pub cart_type: u8,
    pub title: [u8; 16],
}

fn ram_size_bytes(code: u8) -> usize {
    match code {
        0x00 => 0,
        0x01 => 2 * 1024,  // unused in practice, kept for completeness
        0x02 => 8 * 1024,
        0x03 => 32 * 1024,
        0x04 => 128 * 1024,
        0x05 => 64 * 1024,
        _ => 32 * 1024,
    }
}

impl Cart {
    pub fn new(rom: Vec<u8>) -> Cart {
        let mut title = [0u8; 16];
        for i in 0..16 {
            if 0x134 + i < rom.len() {
                let b = rom[0x134 + i];
                title[i] = if b >= 0x20 && b < 0x7F { b } else { 0 };
            }
        }
        let cart_type = if rom.len() > 0x147 { rom[0x147] } else { 0 };
        let ram_code = if rom.len() > 0x149 { rom[0x149] } else { 0 };
        let (kind, has_battery, has_rtc) = match cart_type {
            0x00 => (MbcKind::None, false, false),
            0x01 | 0x02 => (MbcKind::Mbc1, false, false),
            0x03 => (MbcKind::Mbc1, true, false),
            0x0F => (MbcKind::Mbc3, true, true),
            0x10 => (MbcKind::Mbc3, true, true),
            0x11 | 0x12 => (MbcKind::Mbc3, false, false),
            0x13 => (MbcKind::Mbc3, true, false),
            0x08 | 0x09 => (MbcKind::None, false, false), // ROM+RAM(+BATTERY), no banking
            // MBC5: the mapper Pokemon Yellow (cart type 0x1B) actually uses.
            // 0x19 plain, 0x1A/1D +RAM, 0x1B/1E +RAM+BATTERY, 0x1C/1D/1E also
            // have a rumble motor (irrelevant here: no rumble output exists).
            0x19 | 0x1C => (MbcKind::Mbc5, false, false),
            0x1A | 0x1D => (MbcKind::Mbc5, false, false),
            0x1B | 0x1E => (MbcKind::Mbc5, true, false),
            _ => (MbcKind::Unknown, false, false),
        };
        let ram_bytes = if cart_type == 0x08 || cart_type == 0x09 {
            8 * 1024
        } else {
            ram_size_bytes(ram_code)
        };
        let num_rom_banks = (rom.len() as u32 / 0x4000).max(2);
        Cart {
            rom,
            ram: alloc::vec![0u8; ram_bytes.max(1)],
            kind,
            rom_bank_lo: 1,
            rom_bank_hi9: 0,
            bank_hi: 0,
            ram_enabled: false,
            mbc1_mode: 0,
            has_battery,
            has_rtc,
            rtc: [0; 5],
            rtc_latched: [0; 5],
            rtc_latch_prev: 0xFF,
            rtc_sub_cycles: 0,
            num_rom_banks,
            cart_type,
            title,
        }
    }

    fn rom_bank_number(&self) -> u32 {
        match self.kind {
            MbcKind::None => 1,
            MbcKind::Mbc1 => {
                let lo = (self.rom_bank_lo & 0x1F) as u32;
                let lo = if lo == 0 { 1 } else { lo };
                if self.mbc1_mode == 0 {
                    lo | ((self.bank_hi as u32 & 0x03) << 5)
                } else {
                    lo
                }
            }
            MbcKind::Mbc3 | MbcKind::Unknown => {
                let lo = (self.rom_bank_lo & 0x7F) as u32;
                if lo == 0 {
                    1
                } else {
                    lo
                }
            }
            // MBC5 is the one mapper on this list with NO "bank 0 means 1"
            // substitution: a bank register value of 0 genuinely selects ROM
            // bank 0 in the switchable window. 9-bit bank number (0-511).
            MbcKind::Mbc5 => ((self.rom_bank_hi9 as u32) << 8) | self.rom_bank_lo as u32,
        }
    }

    fn ram_bank_number(&self) -> u32 {
        match self.kind {
            MbcKind::Mbc1 if self.mbc1_mode == 1 => (self.bank_hi & 0x03) as u32,
            MbcKind::Mbc3 | MbcKind::Unknown => (self.bank_hi & 0x03) as u32,
            MbcKind::Mbc5 => (self.bank_hi & 0x0F) as u32,
            _ => 0,
        }
    }

    pub fn read8(&self, addr: u16) -> u8 {
        match addr {
            0x0000..=0x3FFF => {
                // MBC1 mode-1 has a quirk where the fixed bank-0 window is also
                // affected by the upper bank bits on >=1MB carts. Not modelled
                // (noted limitation); Pokemon-sized carts (1-2MB, MBC3) do not
                // depend on it since MBC3 has no such mode.
                *self.rom.get(addr as usize).unwrap_or(&0xFF)
            }
            0x4000..=0x7FFF => {
                let bank = self.rom_bank_number() % self.num_rom_banks.max(1);
                let off = bank as usize * 0x4000 + (addr as usize - 0x4000);
                *self.rom.get(off).unwrap_or(&0xFF)
            }
            0xA000..=0xBFFF => {
                if !self.ram_enabled {
                    return 0xFF;
                }
                if self.has_rtc && self.bank_hi >= 0x08 && self.bank_hi <= 0x0C {
                    return self.rtc_latched[(self.bank_hi - 0x08) as usize];
                }
                let bank = self.ram_bank_number() as usize;
                let idx = bank * 0x2000 + (addr as usize - 0xA000);
                *self.ram.get(idx).unwrap_or(&0xFF)
            }
            _ => 0xFF,
        }
    }

    pub fn write8(&mut self, addr: u16, val: u8) {
        match addr {
            0x0000..=0x1FFF => self.ram_enabled = (val & 0x0F) == 0x0A,
            0x2000..=0x3FFF => match self.kind {
                MbcKind::Mbc1 => self.rom_bank_lo = val & 0x1F,
                MbcKind::Mbc3 | MbcKind::Unknown => self.rom_bank_lo = val & 0x7F,
                // MBC5 splits this range in two: 0x2000-0x2FFF is the low 8
                // bits of the bank number, 0x3000-0x3FFF is bit 8 (only bit 0
                // of the written byte matters).
                MbcKind::Mbc5 => {
                    if addr < 0x3000 {
                        self.rom_bank_lo = val;
                    } else {
                        self.rom_bank_hi9 = val & 1;
                    }
                }
                MbcKind::None => {}
            },
            0x4000..=0x5FFF => self.bank_hi = val,
            0x6000..=0x7FFF => match self.kind {
                MbcKind::Mbc1 => self.mbc1_mode = val & 1,
                MbcKind::Mbc3 => {
                    // RTC latch: a 0 then 1 write latches the live counter into
                    // the readable copy (Pan Docs "MBC3" RTC section).
                    if self.rtc_latch_prev == 0 && val == 1 {
                        self.rtc_latched = self.rtc;
                    }
                    self.rtc_latch_prev = val;
                }
                _ => {}
            },
            0xA000..=0xBFFF => {
                if !self.ram_enabled {
                    return;
                }
                if self.has_rtc && self.bank_hi >= 0x08 && self.bank_hi <= 0x0C {
                    self.rtc[(self.bank_hi - 0x08) as usize] = val;
                    return;
                }
                let bank = self.ram_bank_number() as usize;
                let idx = bank * 0x2000 + (addr as usize - 0xA000);
                if idx < self.ram.len() {
                    self.ram[idx] = val;
                }
            }
            _ => {}
        }
    }

    // Advance the MBC3 real-time clock. Driven by the emulator's own T-cycle
    // count (the GB clock rate), matching how the real chip's RTC free-runs
    // off the cartridge's 32.768 kHz crystal rather than off any host clock.
    pub fn tick_rtc(&mut self, t_cycles: u32) {
        if !self.has_rtc || (self.rtc[4] & 0x40) != 0 {
            return; // halted (bit 6 of DH)
        }
        self.rtc_sub_cycles += t_cycles;
        const CYCLES_PER_SEC: u32 = 4_194_304;
        while self.rtc_sub_cycles >= CYCLES_PER_SEC {
            self.rtc_sub_cycles -= CYCLES_PER_SEC;
            self.rtc[0] += 1;
            if self.rtc[0] >= 60 {
                self.rtc[0] = 0;
                self.rtc[1] += 1;
                if self.rtc[1] >= 60 {
                    self.rtc[1] = 0;
                    self.rtc[2] += 1;
                    if self.rtc[2] >= 24 {
                        self.rtc[2] = 0;
                        let mut day = ((self.rtc[4] & 1) as u16) << 8 | self.rtc[3] as u16;
                        day += 1;
                        if day > 0x1FF {
                            day = 0;
                            self.rtc[4] |= 0x80; // day carry
                        }
                        self.rtc[3] = day as u8;
                        self.rtc[4] = (self.rtc[4] & !1) | (((day >> 8) & 1) as u8);
                    }
                }
            }
        }
    }
}

// ============================================================================
// PPU
// ============================================================================
pub const SCREEN_W: usize = 160;
pub const SCREEN_H: usize = 144;

pub struct Ppu {
    pub lcdc: u8,
    pub stat: u8,
    pub scy: u8,
    pub scx: u8,
    pub ly: u8,
    pub lyc: u8,
    pub bgp: u8,
    pub obp0: u8,
    pub obp1: u8,
    pub wy: u8,
    pub wx: u8,
    pub vram: [u8; 0x2000],
    pub oam: [u8; 0xA0],
    mode_clock: u32,
    window_line: u8,
    pub fb: Vec<u32>, // SCREEN_W*SCREEN_H, packed 0x00RRGGBB
    pub frame_ready: bool,
    pub vblank_irq: bool,
    pub stat_irq: bool,
}

// Simple 4-shade DMG grayscale ramp (public fact about the shade count, not any
// specific palette artwork): index 0 = lightest, 3 = darkest.
// pub: the ROM navigator (main.rs's nav module) reuses this SAME ramp to
// quantise preview images, rather than declaring its own second copy of the
// GB palette (see the standing "reuse the shared primitive" rule).
pub const SHADE: [u32; 4] = [0x00E0F8D0, 0x0088C070, 0x00346856, 0x00081820];

impl Ppu {
    pub fn new() -> Ppu {
        Ppu {
            lcdc: 0x91,
            stat: 0x85,
            scy: 0,
            scx: 0,
            ly: 0,
            lyc: 0,
            bgp: 0xFC,
            obp0: 0xFF,
            obp1: 0xFF,
            wy: 0,
            wx: 0,
            vram: [0; 0x2000],
            oam: [0; 0xA0],
            mode_clock: 0,
            window_line: 0,
            fb: alloc::vec![SHADE[0]; SCREEN_W * SCREEN_H],
            frame_ready: false,
            vblank_irq: false,
            stat_irq: false,
        }
    }

    fn mode(&self) -> u8 {
        self.stat & 0x03
    }
    fn set_mode(&mut self, m: u8) {
        self.stat = (self.stat & !0x03) | (m & 0x03);
        // STAT interrupt sources for mode 0/1/2 (bits 3/4/5).
        let fire = match m {
            0 => self.stat & 0x08 != 0,
            1 => self.stat & 0x10 != 0,
            2 => self.stat & 0x20 != 0,
            _ => false,
        };
        if fire {
            self.stat_irq = true;
        }
    }

    fn check_lyc(&mut self) {
        if self.ly == self.lyc {
            self.stat |= 0x04;
            if self.stat & 0x40 != 0 {
                self.stat_irq = true;
            }
        } else {
            self.stat &= !0x04;
        }
    }

    pub fn write_reg(&mut self, addr: u16, val: u8) {
        match addr {
            0xFF40 => {
                let was_on = self.lcdc & 0x80 != 0;
                self.lcdc = val;
                if !was_on && val & 0x80 != 0 {
                    self.ly = 0;
                    self.mode_clock = 0;
                    self.window_line = 0;
                    self.set_mode(2);
                } else if was_on && val & 0x80 == 0 {
                    self.ly = 0;
                    self.set_mode(0);
                }
            }
            0xFF41 => self.stat = (self.stat & 0x07) | (val & 0xF8),
            0xFF42 => self.scy = val,
            0xFF43 => self.scx = val,
            0xFF44 => {} // LY is read-only
            0xFF45 => {
                self.lyc = val;
                self.check_lyc();
            }
            0xFF47 => self.bgp = val,
            0xFF48 => self.obp0 = val,
            0xFF49 => self.obp1 = val,
            0xFF4A => self.wy = val,
            0xFF4B => self.wx = val,
            _ => {}
        }
    }
    pub fn read_reg(&self, addr: u16) -> u8 {
        match addr {
            0xFF40 => self.lcdc,
            0xFF41 => self.stat | 0x80,
            0xFF42 => self.scy,
            0xFF43 => self.scx,
            0xFF44 => self.ly,
            0xFF45 => self.lyc,
            0xFF47 => self.bgp,
            0xFF48 => self.obp0,
            0xFF49 => self.obp1,
            0xFF4A => self.wy,
            0xFF4B => self.wx,
            _ => 0xFF,
        }
    }

    // Advance the PPU by t_cycles (T-states). See file header: mode 3 uses a
    // fixed 172-cycle length (a documented simplification) and each scanline
    // is composited in one shot on leaving mode 3, rather than dot-by-dot.
    pub fn step(&mut self, t_cycles: u32) {
        if self.lcdc & 0x80 == 0 {
            return; // LCD off: PPU frozen (real hardware behaviour)
        }
        self.mode_clock += t_cycles;
        loop {
            match self.mode() {
                2 => {
                    if self.mode_clock >= 80 {
                        self.mode_clock -= 80;
                        self.set_mode(3);
                    } else {
                        break;
                    }
                }
                3 => {
                    if self.mode_clock >= 172 {
                        self.mode_clock -= 172;
                        self.render_scanline();
                        self.set_mode(0);
                    } else {
                        break;
                    }
                }
                0 => {
                    if self.mode_clock >= 204 {
                        self.mode_clock -= 204;
                        self.ly += 1;
                        self.check_lyc();
                        if self.ly == 144 {
                            self.set_mode(1);
                            self.vblank_irq = true;
                            self.frame_ready = true;
                        } else {
                            self.set_mode(2);
                        }
                    } else {
                        break;
                    }
                }
                _ => {
                    // mode 1: vblank, 10 lines of 456 cycles each.
                    if self.mode_clock >= 456 {
                        self.mode_clock -= 456;
                        self.ly += 1;
                        if self.ly > 153 {
                            self.ly = 0;
                            self.window_line = 0;
                            self.check_lyc();
                            self.set_mode(2);
                        } else {
                            self.check_lyc();
                        }
                    } else {
                        break;
                    }
                }
            }
        }
    }

    fn bg_pixel(&self, tile_map_base: u16, tx: u8, ty: u8, px: u8, py: u8) -> u8 {
        let map_addr = tile_map_base + (ty as u16 / 8) * 32 + (tx as u16 / 8);
        let tile_num = self.vram[(map_addr - 0x8000) as usize];
        let unsigned_mode = self.lcdc & 0x10 != 0;
        let tile_addr: u16 = if unsigned_mode {
            0x8000 + (tile_num as u16) * 16
        } else {
            let signed = tile_num as i8 as i32;
            (0x9000i32 + signed * 16) as u16
        };
        let row = (py % 8) as u16;
        let lo = self.vram[(tile_addr - 0x8000 + row * 2) as usize];
        let hi = self.vram[(tile_addr - 0x8000 + row * 2 + 1) as usize];
        let bit = 7 - (px % 8);
        (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1)
    }

    fn apply_palette(pal: u8, idx: u8) -> u32 {
        let shade = (pal >> (idx * 2)) & 0x03;
        SHADE[shade as usize]
    }

    fn render_scanline(&mut self) {
        let ly = self.ly;
        if ly as usize >= SCREEN_H {
            return;
        }
        let bg_enable = self.lcdc & 0x01 != 0;
        let win_enable = self.lcdc & 0x20 != 0 && self.wy <= ly;
        let bg_map: u16 = if self.lcdc & 0x08 != 0 { 0x9C00 } else { 0x9800 };
        let win_map: u16 = if self.lcdc & 0x40 != 0 { 0x9C00 } else { 0x9800 };
        let mut bg_color_idx = [0u8; SCREEN_W];
        let mut used_window_this_line = false;

        for x in 0..SCREEN_W {
            let mut idx = 0u8;
            let wx_px = self.wx as i32 - 7;
            if win_enable && (x as i32) >= wx_px {
                used_window_this_line = true;
                let wtx = (x as i32 - wx_px) as u8;
                idx = self.bg_pixel(win_map, wtx, self.window_line, wtx, self.window_line);
            } else if bg_enable {
                let sx = self.scx.wrapping_add(x as u8);
                let sy = self.scy.wrapping_add(ly);
                idx = self.bg_pixel(bg_map, sx, sy, sx, sy);
            }
            bg_color_idx[x] = idx;
            let color = if bg_enable || win_enable {
                Self::apply_palette(self.bgp, idx)
            } else {
                SHADE[0]
            };
            self.fb[ly as usize * SCREEN_W + x] = color;
        }
        if used_window_this_line {
            self.window_line = self.window_line.wrapping_add(1);
        }

        if self.lcdc & 0x02 != 0 {
            self.render_sprites(ly, &bg_color_idx);
        }
    }

    fn render_sprites(&mut self, ly: u8, bg_color_idx: &[u8; SCREEN_W]) {
        let tall = self.lcdc & 0x04 != 0;
        let height: i32 = if tall { 16 } else { 8 };
        // Collect up to 10 sprites intersecting this line, preserving OAM order
        // as the tie-break (matches DMG hardware priority: lower OAM index wins
        // on equal X).
        let mut picked: [usize; 10] = [0; 10];
        let mut n = 0usize;
        for i in 0..40 {
            if n >= 10 {
                break;
            }
            let base = i * 4;
            let y = self.oam[base] as i32 - 16;
            if (ly as i32) >= y && (ly as i32) < y + height {
                picked[n] = i;
                n += 1;
            }
        }
        // DMG priority: smaller X drawn on top; equal X, smaller OAM index on
        // top. Draw back-to-front, i.e. iterate in REVERSE priority order.
        let mut order: [usize; 10] = picked;
        for a in 0..n {
            for b in (a + 1)..n {
                let xa = self.oam[order[a] * 4 + 1];
                let xb = self.oam[order[b] * 4 + 1];
                // We want ascending priority order for back-to-front drawing:
                // draw LOWEST priority (larger X, or equal X larger OAM idx)
                // first, so highest priority ends up drawn last (on top).
                if xb > xa || (xb == xa && order[b] < order[a]) {
                    order.swap(a, b);
                }
            }
        }
        for k in 0..n {
            let i = order[k];
            let base = i * 4;
            let y = self.oam[base] as i32 - 16;
            let x = self.oam[base + 1] as i32 - 8;
            let mut tile = self.oam[base + 2];
            let attr = self.oam[base + 3];
            let x_flip = attr & 0x20 != 0;
            let y_flip = attr & 0x40 != 0;
            let behind_bg = attr & 0x80 != 0;
            let palette = if attr & 0x10 != 0 { self.obp1 } else { self.obp0 };
            let mut row = ly as i32 - y;
            if y_flip {
                row = height - 1 - row;
            }
            if tall {
                tile &= 0xFE;
                if row >= 8 {
                    tile |= 1;
                    row -= 8;
                }
            }
            let tile_addr = 0x8000u16 + tile as u16 * 16;
            let lo = self.vram[(tile_addr - 0x8000 + row as u16 * 2) as usize];
            let hi = self.vram[(tile_addr - 0x8000 + row as u16 * 2 + 1) as usize];
            for px in 0..8i32 {
                let sx = x + px;
                if sx < 0 || sx as usize >= SCREEN_W {
                    continue;
                }
                let bit = if x_flip { px } else { 7 - px };
                let cidx = (((hi >> bit) & 1) << 1) | ((lo >> bit) & 1);
                if cidx == 0 {
                    continue; // transparent
                }
                if behind_bg && bg_color_idx[sx as usize] != 0 {
                    continue;
                }
                self.fb[ly as usize * SCREEN_W + sx as usize] = Self::apply_palette(palette, cidx);
            }
        }
    }
}

// ============================================================================
// Joypad
// ============================================================================
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Button {
    Right,
    Left,
    Up,
    Down,
    A,
    B,
    Select,
    Start,
}

pub struct Joypad {
    select_bits: u8, // bits 4-5 as written by the game (0 = that group selected)
    dpad: u8,        // bit0 right,1 left,2 up,3 down (1 = pressed)
    action: u8,      // bit0 A,1 B,2 select,3 start (1 = pressed)
    pub irq: bool,
}

impl Joypad {
    pub fn new() -> Joypad {
        Joypad { select_bits: 0x30, dpad: 0, action: 0, irq: false }
    }
    pub fn write(&mut self, val: u8) {
        self.select_bits = val & 0x30;
    }
    pub fn read(&self) -> u8 {
        let mut lo = 0x0F;
        if self.select_bits & 0x10 == 0 {
            lo &= !self.dpad;
        }
        if self.select_bits & 0x20 == 0 {
            lo &= !self.action;
        }
        0xC0 | self.select_bits | lo
    }
    pub fn set(&mut self, b: Button, pressed: bool) {
        let (field, bit): (&mut u8, u8) = match b {
            Button::Right => (&mut self.dpad, 0),
            Button::Left => (&mut self.dpad, 1),
            Button::Up => (&mut self.dpad, 2),
            Button::Down => (&mut self.dpad, 3),
            Button::A => (&mut self.action, 0),
            Button::B => (&mut self.action, 1),
            Button::Select => (&mut self.action, 2),
            Button::Start => (&mut self.action, 3),
        };
        let was = *field & (1 << bit) != 0;
        if pressed {
            *field |= 1 << bit;
        } else {
            *field &= !(1 << bit);
        }
        // Simplification (noted in file header): request the joypad interrupt
        // on any press regardless of which group is currently selected. Real
        // hardware only does this for the selected group; games (including
        // Pokemon) use this interrupt only to wake from STOP/HALT on the
        // title/menu, where this is equivalent in practice.
        if pressed && !was {
            self.irq = true;
        }
    }
}

// ============================================================================
// Timer (DIV / TIMA / TMA / TAC)
// ============================================================================
pub struct Timer {
    pub counter: u16, // internal 16-bit free-running counter; DIV = high byte
    pub tima: u8,
    pub tma: u8,
    pub tac: u8,
    pub irq: bool,
    overflow_delay: i8, // -1 = none; counts down to the delayed reload (Pan Docs TIMA quirk)
}

const TAC_BIT: [u8; 4] = [9, 3, 5, 7];

impl Timer {
    pub fn new() -> Timer {
        Timer { counter: 0xAB00, tima: 0, tma: 0, tac: 0xF8, irq: false, overflow_delay: -1 }
    }
    fn selected_bit_set(&self) -> bool {
        (self.tac & 0x04 != 0) && (self.counter & (1 << TAC_BIT[(self.tac & 0x03) as usize]) != 0)
    }
    pub fn write_div(&mut self) {
        let before = self.selected_bit_set();
        self.counter = 0;
        if before {
            self.tick_tima_edge();
        }
    }
    pub fn write_tac(&mut self, val: u8) {
        let before = self.selected_bit_set();
        self.tac = val;
        let after = self.selected_bit_set();
        if before && !after {
            self.tick_tima_edge();
        }
    }
    fn tick_tima_edge(&mut self) {
        let (v, of) = self.tima.overflowing_add(1);
        self.tima = v;
        if of {
            // Real hardware delays the TMA reload / interrupt by 1 M-cycle
            // (4 T-cycles): TIMA reads as 0x00 for that cycle. Modelled here.
            self.overflow_delay = 4;
        }
    }
    pub fn step(&mut self, t_cycles: u32) {
        for _ in 0..t_cycles {
            let before = self.selected_bit_set();
            self.counter = self.counter.wrapping_add(1);
            let after = self.selected_bit_set();
            if before && !after {
                self.tick_tima_edge();
            }
            if self.overflow_delay > 0 {
                self.overflow_delay -= 1;
                if self.overflow_delay == 0 {
                    self.tima = self.tma;
                    self.irq = true;
                    self.overflow_delay = -1;
                }
            }
        }
    }
}

// ============================================================================
// CPU
// ============================================================================
pub struct Cpu {
    pub r: Regs,
    pub ime: bool,
    pub ei_delay: u8,
    pub halted: bool,
    pub halt_bug: bool,
    pub stopped: bool,
}

impl Cpu {
    pub fn new() -> Cpu {
        // Post-boot-ROM register state (public "Power Up Sequence" facts).
        Cpu {
            r: Regs { a: 0x01, f: 0xB0, b: 0x00, c: 0x13, d: 0x00, e: 0xD8, h: 0x01, l: 0x4D, sp: 0xFFFE, pc: 0x0100 },
            ime: false,
            ei_delay: 0,
            halted: false,
            halt_bug: false,
            stopped: false,
        }
    }
}

// ============================================================================
// The whole machine
// ============================================================================
pub struct Gb {
    pub cpu: Cpu,
    pub cart: Cart,
    pub ppu: Ppu,
    pub timer: Timer,
    pub joyp: Joypad,
    pub wram: [u8; 0x2000],
    pub hram: [u8; 0x7F],
    pub ie: u8,
    pub iflag: u8,
    pub sb: u8,
    pub sc: u8,
    serial_shift_remaining: u32,
    audio_regs: [u8; 0x30],
    pub apu: Apu,
    pub serial_log: Vec<u8>,
    pub total_m_cycles: u64,
}

// ===================== Game Boy APU (audio) =====================
// Clocked from Gb::step() by T-cycles; emits interleaved stereo i16 at APU_RATE
// into `out`, drained each frame by the front end and pushed to the OS PCM sink.
// Built from the Pan Docs APU spec: 2 square channels (ch1 with frequency sweep),
// a 32-sample wave channel, and an LFSR noise channel, a 512 Hz frame sequencer
// driving length/envelope/sweep, NR51 panning and NR50 master volume.
pub const APU_RATE: u32 = 44100;
const APU_GB_CLOCK: u32 = 4194304;

const DUTY: [[u8; 8]; 4] = [
    [0, 0, 0, 0, 0, 0, 0, 1],
    [1, 0, 0, 0, 0, 0, 0, 1],
    [1, 0, 0, 0, 0, 1, 1, 1],
    [0, 1, 1, 1, 1, 1, 1, 0],
];

struct Square {
    has_sweep: bool,
    enabled: bool,
    dac_on: bool,
    duty: u8,
    freq: u16,
    len_enable: bool,
    len_timer: u16,
    env_vol0: u8,
    env_add: bool,
    env_period: u8,
    sw_period: u8,
    sw_neg: bool,
    sw_shift: u8,
    timer: i32,
    duty_pos: u8,
    vol: u8,
    env_timer: u8,
    sw_timer: u8,
    sw_enabled: bool,
    sw_shadow: u16,
}
impl Square {
    fn new(has_sweep: bool) -> Square {
        Square {
            has_sweep,
            enabled: false,
            dac_on: false,
            duty: 0,
            freq: 0,
            len_enable: false,
            len_timer: 0,
            env_vol0: 0,
            env_add: false,
            env_period: 0,
            sw_period: 0,
            sw_neg: false,
            sw_shift: 0,
            timer: 0,
            duty_pos: 0,
            vol: 0,
            env_timer: 0,
            sw_timer: 0,
            sw_enabled: false,
            sw_shadow: 0,
        }
    }
    fn period(&self) -> i32 {
        (2048 - self.freq as i32) * 4
    }
    fn sweep_calc(&mut self) -> u16 {
        let d = self.sw_shadow >> self.sw_shift;
        let nf = if self.sw_neg {
            self.sw_shadow.wrapping_sub(d)
        } else {
            self.sw_shadow.wrapping_add(d)
        };
        if nf > 2047 {
            self.enabled = false;
        }
        nf
    }
    fn trigger(&mut self) {
        self.enabled = self.dac_on;
        if self.len_timer == 0 {
            self.len_timer = 64;
        }
        self.timer = self.period();
        self.vol = self.env_vol0;
        self.env_timer = if self.env_period == 0 { 8 } else { self.env_period };
        if self.has_sweep {
            self.sw_shadow = self.freq;
            self.sw_timer = if self.sw_period == 0 { 8 } else { self.sw_period };
            self.sw_enabled = self.sw_period != 0 || self.sw_shift != 0;
            if self.sw_shift != 0 {
                let _ = self.sweep_calc();
            }
        }
    }
    fn step(&mut self, t: i32) {
        if !self.enabled {
            return;
        }
        self.timer -= t;
        let mut guard = 0;
        while self.timer <= 0 {
            self.timer += self.period();
            self.duty_pos = (self.duty_pos + 1) & 7;
            guard += 1;
            if guard > 64 {
                break;
            }
        }
    }
    fn clock_len(&mut self) {
        if self.len_enable && self.len_timer > 0 {
            self.len_timer -= 1;
            if self.len_timer == 0 {
                self.enabled = false;
            }
        }
    }
    fn clock_env(&mut self) {
        if self.env_period == 0 {
            return;
        }
        if self.env_timer > 0 {
            self.env_timer -= 1;
        }
        if self.env_timer == 0 {
            self.env_timer = self.env_period;
            if self.env_add && self.vol < 15 {
                self.vol += 1;
            } else if !self.env_add && self.vol > 0 {
                self.vol -= 1;
            }
        }
    }
    fn clock_sweep(&mut self) {
        if !self.has_sweep || !self.sw_enabled {
            return;
        }
        if self.sw_timer > 0 {
            self.sw_timer -= 1;
        }
        if self.sw_timer == 0 {
            self.sw_timer = if self.sw_period == 0 { 8 } else { self.sw_period };
            if self.sw_period != 0 {
                let nf = self.sweep_calc();
                if nf <= 2047 && self.sw_shift != 0 {
                    self.sw_shadow = nf;
                    self.freq = nf;
                    let _ = self.sweep_calc();
                }
            }
        }
    }
    fn out(&self) -> u8 {
        if !self.enabled || !self.dac_on {
            return 0;
        }
        if DUTY[self.duty as usize][self.duty_pos as usize] != 0 {
            self.vol
        } else {
            0
        }
    }
}

struct Wave {
    enabled: bool,
    dac_on: bool,
    freq: u16,
    len_enable: bool,
    len_timer: u16,
    vol_shift: u8,
    timer: i32,
    pos: u8,
    ram: [u8; 16],
    sample: u8,
}
impl Wave {
    fn new() -> Wave {
        Wave {
            enabled: false,
            dac_on: false,
            freq: 0,
            len_enable: false,
            len_timer: 0,
            vol_shift: 0,
            timer: 0,
            pos: 0,
            ram: [0; 16],
            sample: 0,
        }
    }
    fn period(&self) -> i32 {
        (2048 - self.freq as i32) * 2
    }
    fn trigger(&mut self) {
        self.enabled = self.dac_on;
        if self.len_timer == 0 {
            self.len_timer = 256;
        }
        self.timer = self.period();
        self.pos = 0;
    }
    fn step(&mut self, t: i32) {
        if !self.enabled {
            return;
        }
        self.timer -= t;
        let mut guard = 0;
        while self.timer <= 0 {
            self.timer += self.period();
            self.pos = (self.pos + 1) & 31;
            let byte = self.ram[(self.pos >> 1) as usize];
            self.sample = if self.pos & 1 == 0 { byte >> 4 } else { byte & 0x0F };
            guard += 1;
            if guard > 64 {
                break;
            }
        }
    }
    fn clock_len(&mut self) {
        if self.len_enable && self.len_timer > 0 {
            self.len_timer -= 1;
            if self.len_timer == 0 {
                self.enabled = false;
            }
        }
    }
    fn out(&self) -> u8 {
        if !self.enabled || !self.dac_on || self.vol_shift == 0 {
            return 0;
        }
        self.sample >> (self.vol_shift - 1)
    }
}

const NOISE_DIV: [i32; 8] = [8, 16, 32, 48, 64, 80, 96, 112];

struct Noise {
    enabled: bool,
    dac_on: bool,
    len_enable: bool,
    len_timer: u16,
    env_vol0: u8,
    env_add: bool,
    env_period: u8,
    div_code: u8,
    width7: bool,
    shift: u8,
    timer: i32,
    lfsr: u16,
    vol: u8,
    env_timer: u8,
}
impl Noise {
    fn new() -> Noise {
        Noise {
            enabled: false,
            dac_on: false,
            len_enable: false,
            len_timer: 0,
            env_vol0: 0,
            env_add: false,
            env_period: 0,
            div_code: 0,
            width7: false,
            shift: 0,
            timer: 0,
            lfsr: 0x7FFF,
            vol: 0,
            env_timer: 0,
        }
    }
    fn period(&self) -> i32 {
        let d = NOISE_DIV[(self.div_code & 7) as usize];
        d << self.shift
    }
    fn trigger(&mut self) {
        self.enabled = self.dac_on;
        if self.len_timer == 0 {
            self.len_timer = 64;
        }
        self.timer = self.period();
        self.lfsr = 0x7FFF;
        self.vol = self.env_vol0;
        self.env_timer = if self.env_period == 0 { 8 } else { self.env_period };
    }
    fn step(&mut self, t: i32) {
        if !self.enabled {
            return;
        }
        self.timer -= t;
        let mut guard = 0;
        while self.timer <= 0 {
            let p = self.period();
            self.timer += if p <= 0 { 8 } else { p };
            let bit = (self.lfsr & 1) ^ ((self.lfsr >> 1) & 1);
            self.lfsr = (self.lfsr >> 1) | (bit << 14);
            if self.width7 {
                self.lfsr = (self.lfsr & !(1 << 6)) | (bit << 6);
            }
            guard += 1;
            if guard > 128 {
                break;
            }
        }
    }
    fn clock_len(&mut self) {
        if self.len_enable && self.len_timer > 0 {
            self.len_timer -= 1;
            if self.len_timer == 0 {
                self.enabled = false;
            }
        }
    }
    fn clock_env(&mut self) {
        if self.env_period == 0 {
            return;
        }
        if self.env_timer > 0 {
            self.env_timer -= 1;
        }
        if self.env_timer == 0 {
            self.env_timer = self.env_period;
            if self.env_add && self.vol < 15 {
                self.vol += 1;
            } else if !self.env_add && self.vol > 0 {
                self.vol -= 1;
            }
        }
    }
    fn out(&self) -> u8 {
        if !self.enabled || !self.dac_on {
            return 0;
        }
        if self.lfsr & 1 == 0 {
            self.vol
        } else {
            0
        }
    }
}

pub struct Apu {
    power: bool,
    ch1: Square,
    ch2: Square,
    ch3: Wave,
    ch4: Noise,
    nr50: u8,
    nr51: u8,
    fs_counter: u32,
    fs_step: u8,
    sample_acc: u32,
    hp_l: i32,
    hp_r: i32,
    prev_l_in: i32,
    prev_r_in: i32,
    pub out: Vec<i16>,
}
impl Apu {
    pub fn new() -> Apu {
        Apu {
            power: false,
            ch1: Square::new(true),
            ch2: Square::new(false),
            ch3: Wave::new(),
            ch4: Noise::new(),
            nr50: 0,
            nr51: 0,
            fs_counter: 0,
            fs_step: 0,
            sample_acc: 0,
            hp_l: 0,
            hp_r: 0,
            prev_l_in: 0,
            prev_r_in: 0,
            out: Vec::with_capacity(2048),
        }
    }

    fn frame_seq(&mut self) {
        // 512 Hz sequencer: len at 256 Hz (even steps), sweep at 128 Hz (2,6),
        // envelope at 64 Hz (step 7).
        match self.fs_step {
            0 | 4 => {
                self.ch1.clock_len();
                self.ch2.clock_len();
                self.ch3.clock_len();
                self.ch4.clock_len();
            }
            2 | 6 => {
                self.ch1.clock_len();
                self.ch2.clock_len();
                self.ch3.clock_len();
                self.ch4.clock_len();
                self.ch1.clock_sweep();
            }
            7 => {
                self.ch1.clock_env();
                self.ch2.clock_env();
                self.ch4.clock_env();
            }
            _ => {}
        }
        self.fs_step = (self.fs_step + 1) & 7;
    }

    pub fn step(&mut self, t: u32) {
        if !self.power {
            // keep emitting silence so the sink stays paced and drains
            self.sample_acc += t * APU_RATE;
            while self.sample_acc >= APU_GB_CLOCK {
                self.sample_acc -= APU_GB_CLOCK;
                self.out.push(0);
                self.out.push(0);
            }
            return;
        }
        let ti = t as i32;
        self.ch1.step(ti);
        self.ch2.step(ti);
        self.ch3.step(ti);
        self.ch4.step(ti);

        self.fs_counter += t;
        while self.fs_counter >= 8192 {
            self.fs_counter -= 8192;
            self.frame_seq();
        }

        self.sample_acc += t * APU_RATE;
        while self.sample_acc >= APU_GB_CLOCK {
            self.sample_acc -= APU_GB_CLOCK;
            self.emit_sample();
        }
    }

    fn emit_sample(&mut self) {
        let o1 = self.ch1.out() as i32;
        let o2 = self.ch2.out() as i32;
        let o3 = self.ch3.out() as i32;
        let o4 = self.ch4.out() as i32;
        let mut l = 0i32;
        let mut r = 0i32;
        // NR51: bits 4-7 = left enables (ch4..ch1), bits 0-3 = right enables.
        if self.nr51 & 0x10 != 0 {
            l += o1;
        }
        if self.nr51 & 0x20 != 0 {
            l += o2;
        }
        if self.nr51 & 0x40 != 0 {
            l += o3;
        }
        if self.nr51 & 0x80 != 0 {
            l += o4;
        }
        if self.nr51 & 0x01 != 0 {
            r += o1;
        }
        if self.nr51 & 0x02 != 0 {
            r += o2;
        }
        if self.nr51 & 0x04 != 0 {
            r += o3;
        }
        if self.nr51 & 0x08 != 0 {
            r += o4;
        }
        // NR50 master volume 0..7 per side (+1).
        let lv = ((self.nr50 >> 4) & 7) as i32 + 1;
        let rv = (self.nr50 & 7) as i32 + 1;
        // Each channel 0..15, up to 4 summed = 0..60, times vol(1..8).
        // Scale to i16: /(60*8) then * a headroomed amplitude.
        // Scale the 0..480 unsigned mix up into the i16 range with headroom.
        let li = l * lv * 55;
        let ri = r * rv * 55;
        // One-pole DC blocker (pole ~0.999): centres the unsigned mix so there is
        // no DC bias and no start/stop click, and the full range is used.
        self.hp_l = li - self.prev_l_in + self.hp_l * 1023 / 1024;
        self.prev_l_in = li;
        self.hp_r = ri - self.prev_r_in + self.hp_r * 1023 / 1024;
        self.prev_r_in = ri;
        let cl = |x: i32| -> i16 { if x > 32767 { 32767 } else if x < -32768 { -32768 } else { x as i16 } };
        self.out.push(cl(self.hp_l));
        self.out.push(cl(self.hp_r));
    }

    pub fn write_reg(&mut self, addr: u16, val: u8) {
        // Wave RAM is writable regardless of power.
        if (0xFF30..=0xFF3F).contains(&addr) {
            self.ch3.ram[(addr - 0xFF30) as usize] = val;
            return;
        }
        if addr == 0xFF26 {
            let on = val & 0x80 != 0;
            if !on && self.power {
                // power off: reset channel enables
                self.ch1.enabled = false;
                self.ch2.enabled = false;
                self.ch3.enabled = false;
                self.ch4.enabled = false;
            }
            self.power = on;
            return;
        }
        if !self.power {
            return;
        }
        match addr {
            0xFF10 => {
                self.ch1.sw_period = (val >> 4) & 7;
                self.ch1.sw_neg = val & 0x08 != 0;
                self.ch1.sw_shift = val & 7;
            }
            0xFF11 => {
                self.ch1.duty = (val >> 6) & 3;
                self.ch1.len_timer = 64 - (val & 0x3F) as u16;
            }
            0xFF12 => {
                self.ch1.env_vol0 = (val >> 4) & 0x0F;
                self.ch1.env_add = val & 0x08 != 0;
                self.ch1.env_period = val & 7;
                self.ch1.dac_on = val & 0xF8 != 0;
                if !self.ch1.dac_on {
                    self.ch1.enabled = false;
                }
            }
            0xFF13 => self.ch1.freq = (self.ch1.freq & 0x700) | val as u16,
            0xFF14 => {
                self.ch1.freq = (self.ch1.freq & 0xFF) | (((val & 7) as u16) << 8);
                self.ch1.len_enable = val & 0x40 != 0;
                if val & 0x80 != 0 {
                    self.ch1.trigger();
                }
            }
            0xFF16 => {
                self.ch2.duty = (val >> 6) & 3;
                self.ch2.len_timer = 64 - (val & 0x3F) as u16;
            }
            0xFF17 => {
                self.ch2.env_vol0 = (val >> 4) & 0x0F;
                self.ch2.env_add = val & 0x08 != 0;
                self.ch2.env_period = val & 7;
                self.ch2.dac_on = val & 0xF8 != 0;
                if !self.ch2.dac_on {
                    self.ch2.enabled = false;
                }
            }
            0xFF18 => self.ch2.freq = (self.ch2.freq & 0x700) | val as u16,
            0xFF19 => {
                self.ch2.freq = (self.ch2.freq & 0xFF) | (((val & 7) as u16) << 8);
                self.ch2.len_enable = val & 0x40 != 0;
                if val & 0x80 != 0 {
                    self.ch2.trigger();
                }
            }
            0xFF1A => {
                self.ch3.dac_on = val & 0x80 != 0;
                if !self.ch3.dac_on {
                    self.ch3.enabled = false;
                }
            }
            0xFF1B => self.ch3.len_timer = 256 - val as u16,
            0xFF1C => self.ch3.vol_shift = (val >> 5) & 3,
            0xFF1D => self.ch3.freq = (self.ch3.freq & 0x700) | val as u16,
            0xFF1E => {
                self.ch3.freq = (self.ch3.freq & 0xFF) | (((val & 7) as u16) << 8);
                self.ch3.len_enable = val & 0x40 != 0;
                if val & 0x80 != 0 {
                    self.ch3.trigger();
                }
            }
            0xFF20 => self.ch4.len_timer = 64 - (val & 0x3F) as u16,
            0xFF21 => {
                self.ch4.env_vol0 = (val >> 4) & 0x0F;
                self.ch4.env_add = val & 0x08 != 0;
                self.ch4.env_period = val & 7;
                self.ch4.dac_on = val & 0xF8 != 0;
                if !self.ch4.dac_on {
                    self.ch4.enabled = false;
                }
            }
            0xFF22 => {
                self.ch4.shift = (val >> 4) & 0x0F;
                self.ch4.width7 = val & 0x08 != 0;
                self.ch4.div_code = val & 7;
            }
            0xFF23 => {
                self.ch4.len_enable = val & 0x40 != 0;
                if val & 0x80 != 0 {
                    self.ch4.trigger();
                }
            }
            0xFF24 => self.nr50 = val,
            0xFF25 => self.nr51 = val,
            _ => {}
        }
    }

    pub fn read_nr52(&self) -> u8 {
        let mut v = 0x70u8;
        if self.power {
            v |= 0x80;
        }
        if self.ch1.enabled {
            v |= 1;
        }
        if self.ch2.enabled {
            v |= 2;
        }
        if self.ch3.enabled {
            v |= 4;
        }
        if self.ch4.enabled {
            v |= 8;
        }
        v
    }
}

impl Gb {
    pub fn new(rom: Vec<u8>) -> Gb {
        Gb {
            cpu: Cpu::new(),
            cart: Cart::new(rom),
            ppu: Ppu::new(),
            timer: Timer::new(),
            joyp: Joypad::new(),
            wram: [0; 0x2000],
            hram: [0; 0x7F],
            ie: 0,
            iflag: 0xE1,
            sb: 0,
            sc: 0x7E,
            serial_shift_remaining: 0,
            audio_regs: [0; 0x30],
            apu: Apu::new(),
            serial_log: Vec::new(),
            total_m_cycles: 0,
        }
    }

    pub fn title(&self) -> &[u8; 16] {
        &self.cart.title
    }

    // ---- memory map -------------------------------------------------------
    pub fn read8(&self, addr: u16) -> u8 {
        match addr {
            0x0000..=0x7FFF => self.cart.read8(addr),
            0x8000..=0x9FFF => self.ppu.vram[(addr - 0x8000) as usize],
            0xA000..=0xBFFF => self.cart.read8(addr),
            0xC000..=0xDFFF => self.wram[(addr - 0xC000) as usize],
            0xE000..=0xFDFF => self.wram[(addr - 0xE000) as usize],
            0xFE00..=0xFE9F => self.ppu.oam[(addr - 0xFE00) as usize],
            0xFEA0..=0xFEFF => 0xFF,
            0xFF00 => self.joyp.read(),
            0xFF01 => self.sb,
            0xFF02 => self.sc | 0x7E,
            0xFF04 => (self.timer.counter >> 8) as u8,
            0xFF05 => self.timer.tima,
            0xFF06 => self.timer.tma,
            0xFF07 => self.timer.tac | 0xF8,
            0xFF0F => self.iflag | 0xE0,
            0xFF26 => self.apu.read_nr52(),
            0xFF10..=0xFF3F => self.audio_regs.get((addr - 0xFF10) as usize).copied().unwrap_or(0xFF),
            0xFF40..=0xFF4B => self.ppu.read_reg(addr),
            0xFF80..=0xFFFE => self.hram[(addr - 0xFF80) as usize],
            0xFFFF => self.ie,
            _ => 0xFF,
        }
    }

    pub fn write8(&mut self, addr: u16, val: u8) {
        match addr {
            0x0000..=0x7FFF => self.cart.write8(addr, val),
            0x8000..=0x9FFF => self.ppu.vram[(addr - 0x8000) as usize] = val,
            0xA000..=0xBFFF => self.cart.write8(addr, val),
            0xC000..=0xDFFF => self.wram[(addr - 0xC000) as usize] = val,
            0xE000..=0xFDFF => self.wram[(addr - 0xE000) as usize] = val,
            0xFE00..=0xFE9F => self.ppu.oam[(addr - 0xFE00) as usize] = val,
            0xFEA0..=0xFEFF => {}
            0xFF00 => self.joyp.write(val),
            0xFF01 => self.sb = val,
            0xFF02 => {
                self.sc = val;
                if val & 0x81 == 0x81 {
                    // Internal-clock transfer requested: capture the byte the
                    // guest wants to send immediately (that IS the payload;
                    // see file header on why this is a legitimate, documented
                    // way to harvest Blargg test-ROM output without a PPU),
                    // then simulate the ~4096-T-cycle transfer time before
                    // clearing the "in progress" bit and raising the serial
                    // interrupt, so a guest that polls SC bit 7 does not spin
                    // forever.
                    self.serial_log.push(self.sb);
                    self.serial_shift_remaining = 4096;
                }
            }
            0xFF04 => self.timer.write_div(),
            0xFF05 => self.timer.tima = val,
            0xFF06 => self.timer.tma = val,
            0xFF07 => self.timer.write_tac(val),
            0xFF0F => self.iflag = val & 0x1F,
            0xFF10..=0xFF3F => {
                if let Some(slot) = self.audio_regs.get_mut((addr - 0xFF10) as usize) {
                    *slot = val;
                }
                self.apu.write_reg(addr, val);
            }
            0xFF46 => {
                // OAM DMA: instantaneous copy (documented simplification; see
                // file header). Source high byte only; real hardware also
                // takes ~160 M-cycles during which only HRAM is CPU-reachable.
                let src = (val as u16) << 8;
                for i in 0..0xA0u16 {
                    let b = self.read8(src + i);
                    self.ppu.oam[i as usize] = b;
                }
            }
            0xFF40..=0xFF4B => self.ppu.write_reg(addr, val),
            0xFF80..=0xFFFE => self.hram[(addr - 0xFF80) as usize] = val,
            0xFFFF => self.ie = val,
            _ => {}
        }
    }
    fn read16(&self, addr: u16) -> u16 {
        self.read8(addr) as u16 | ((self.read8(addr.wrapping_add(1)) as u16) << 8)
    }
    fn write16(&mut self, addr: u16, val: u16) {
        self.write8(addr, val as u8);
        self.write8(addr.wrapping_add(1), (val >> 8) as u8);
    }

    // ---- register-index helpers (bitfield decode, see file header) --------
    fn get_r(&self, idx: u8) -> u8 {
        match idx {
            0 => self.cpu.r.b,
            1 => self.cpu.r.c,
            2 => self.cpu.r.d,
            3 => self.cpu.r.e,
            4 => self.cpu.r.h,
            5 => self.cpu.r.l,
            6 => self.read8(self.cpu.r.hl()),
            _ => self.cpu.r.a,
        }
    }
    fn set_r(&mut self, idx: u8, v: u8) {
        match idx {
            0 => self.cpu.r.b = v,
            1 => self.cpu.r.c = v,
            2 => self.cpu.r.d = v,
            3 => self.cpu.r.e = v,
            4 => self.cpu.r.h = v,
            5 => self.cpu.r.l = v,
            6 => self.write8(self.cpu.r.hl(), v),
            _ => self.cpu.r.a = v,
        }
    }
    fn get_rp(&self, p: u8) -> u16 {
        match p {
            0 => self.cpu.r.bc(),
            1 => self.cpu.r.de(),
            2 => self.cpu.r.hl(),
            _ => self.cpu.r.sp,
        }
    }
    fn set_rp(&mut self, p: u8, v: u16) {
        match p {
            0 => self.cpu.r.set_bc(v),
            1 => self.cpu.r.set_de(v),
            2 => self.cpu.r.set_hl(v),
            _ => self.cpu.r.sp = v,
        }
    }
    fn cc(&self, y: u8) -> bool {
        match y & 3 {
            0 => !self.cpu.r.flag(FLAG_Z),
            1 => self.cpu.r.flag(FLAG_Z),
            2 => !self.cpu.r.flag(FLAG_C),
            _ => self.cpu.r.flag(FLAG_C),
        }
    }

    fn fetch8(&mut self) -> u8 {
        let b = self.read8(self.cpu.r.pc);
        if self.cpu.halt_bug {
            self.cpu.halt_bug = false; // consumed once: PC does not advance
        } else {
            self.cpu.r.pc = self.cpu.r.pc.wrapping_add(1);
        }
        b
    }
    fn fetch16(&mut self) -> u16 {
        let lo = self.fetch8() as u16;
        let hi = self.fetch8() as u16;
        lo | (hi << 8)
    }
    fn push16(&mut self, v: u16) {
        self.cpu.r.sp = self.cpu.r.sp.wrapping_sub(2);
        self.write16(self.cpu.r.sp, v);
    }
    fn pop16(&mut self) -> u16 {
        let v = self.read16(self.cpu.r.sp);
        self.cpu.r.sp = self.cpu.r.sp.wrapping_add(2);
        v
    }

    // ---- ALU helpers --------------------------------------------------------
    fn alu_add(&mut self, b: u8, carry_in: u8) {
        let a = self.cpu.r.a;
        let sum16 = a as u16 + b as u16 + carry_in as u16;
        let h = (a & 0xF) + (b & 0xF) + carry_in > 0xF;
        self.cpu.r.a = sum16 as u8;
        self.cpu.r.set_flag(FLAG_Z, self.cpu.r.a == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, h);
        self.cpu.r.set_flag(FLAG_C, sum16 > 0xFF);
    }
    fn alu_sub(&mut self, b: u8, carry_in: u8, store: bool) -> u8 {
        let a = self.cpu.r.a;
        let diff16 = a as i16 - b as i16 - carry_in as i16;
        let h = ((a & 0xF) as i16 - (b & 0xF) as i16 - carry_in as i16) < 0;
        let res = diff16 as u8;
        self.cpu.r.set_flag(FLAG_Z, res == 0);
        self.cpu.r.set_flag(FLAG_N, true);
        self.cpu.r.set_flag(FLAG_H, h);
        self.cpu.r.set_flag(FLAG_C, diff16 < 0);
        if store {
            self.cpu.r.a = res;
        }
        res
    }
    fn alu_and(&mut self, b: u8) {
        self.cpu.r.a &= b;
        self.cpu.r.set_flag(FLAG_Z, self.cpu.r.a == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, true);
        self.cpu.r.set_flag(FLAG_C, false);
    }
    fn alu_or(&mut self, b: u8) {
        self.cpu.r.a |= b;
        self.cpu.r.set_flag(FLAG_Z, self.cpu.r.a == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, false);
        self.cpu.r.set_flag(FLAG_C, false);
    }
    fn alu_xor(&mut self, b: u8) {
        self.cpu.r.a ^= b;
        self.cpu.r.set_flag(FLAG_Z, self.cpu.r.a == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, false);
        self.cpu.r.set_flag(FLAG_C, false);
    }
    fn alu_inc(&mut self, v: u8) -> u8 {
        let res = v.wrapping_add(1);
        self.cpu.r.set_flag(FLAG_Z, res == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, (v & 0x0F) == 0x0F);
        res
    }
    fn alu_dec(&mut self, v: u8) -> u8 {
        let res = v.wrapping_sub(1);
        self.cpu.r.set_flag(FLAG_Z, res == 0);
        self.cpu.r.set_flag(FLAG_N, true);
        self.cpu.r.set_flag(FLAG_H, (v & 0x0F) == 0);
        res
    }
    fn alu_add16_hl(&mut self, b: u16) {
        let a = self.cpu.r.hl();
        let res = a as u32 + b as u32;
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, (a & 0xFFF) + (b & 0xFFF) > 0xFFF);
        self.cpu.r.set_flag(FLAG_C, res > 0xFFFF);
        self.cpu.r.set_hl(res as u16);
    }
    fn add_sp_e8(&mut self) -> u16 {
        let e = self.fetch8() as i8 as i16;
        let sp = self.cpu.r.sp;
        let res = (sp as i16).wrapping_add(e) as u16;
        // H/C computed on the low byte, per the public flag table for this op.
        let h = (sp & 0x0F) as i16 + (e & 0x0F) > 0x0F;
        let c = (sp & 0xFF) as i16 + (e & 0xFF) > 0xFF;
        self.cpu.r.set_flag(FLAG_Z, false);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, h);
        self.cpu.r.set_flag(FLAG_C, c);
        res
    }

    fn rlc(&mut self, v: u8) -> u8 {
        let c = v & 0x80 != 0;
        let r = v.rotate_left(1);
        self.cpu.r.set_flag(FLAG_C, c);
        r
    }
    fn rrc(&mut self, v: u8) -> u8 {
        let c = v & 0x01 != 0;
        let r = v.rotate_right(1);
        self.cpu.r.set_flag(FLAG_C, c);
        r
    }
    fn rl(&mut self, v: u8) -> u8 {
        let old_c = self.cpu.r.flag(FLAG_C) as u8;
        let c = v & 0x80 != 0;
        let r = (v << 1) | old_c;
        self.cpu.r.set_flag(FLAG_C, c);
        r
    }
    fn rr(&mut self, v: u8) -> u8 {
        let old_c = self.cpu.r.flag(FLAG_C) as u8;
        let c = v & 0x01 != 0;
        let r = (v >> 1) | (old_c << 7);
        self.cpu.r.set_flag(FLAG_C, c);
        r
    }
    fn sla(&mut self, v: u8) -> u8 {
        let c = v & 0x80 != 0;
        self.cpu.r.set_flag(FLAG_C, c);
        v << 1
    }
    fn sra(&mut self, v: u8) -> u8 {
        let c = v & 0x01 != 0;
        let r = (v >> 1) | (v & 0x80);
        self.cpu.r.set_flag(FLAG_C, c);
        r
    }
    fn srl(&mut self, v: u8) -> u8 {
        let c = v & 0x01 != 0;
        self.cpu.r.set_flag(FLAG_C, c);
        v >> 1
    }
    fn swap(&mut self, v: u8) -> u8 {
        self.cpu.r.set_flag(FLAG_C, false);
        (v << 4) | (v >> 4)
    }
    fn zflag_from(&mut self, v: u8) {
        self.cpu.r.set_flag(FLAG_Z, v == 0);
        self.cpu.r.set_flag(FLAG_N, false);
        self.cpu.r.set_flag(FLAG_H, false);
    }

    fn daa(&mut self) {
        let mut a = self.cpu.r.a;
        let n = self.cpu.r.flag(FLAG_N);
        let mut carry = self.cpu.r.flag(FLAG_C);
        let half = self.cpu.r.flag(FLAG_H);
        if !n {
            if carry || a > 0x99 {
                a = a.wrapping_add(0x60);
                carry = true;
            }
            if half || (a & 0x0F) > 0x09 {
                a = a.wrapping_add(0x06);
            }
        } else {
            if carry {
                a = a.wrapping_sub(0x60);
            }
            if half {
                a = a.wrapping_sub(0x06);
            }
        }
        self.cpu.r.a = a;
        self.cpu.r.set_flag(FLAG_Z, a == 0);
        self.cpu.r.set_flag(FLAG_H, false);
        self.cpu.r.set_flag(FLAG_C, carry);
    }

    // ---- interrupts ---------------------------------------------------------
    fn pending_interrupt(&self) -> Option<u8> {
        let bits = self.ie & self.iflag & 0x1F;
        if bits == 0 {
            return None;
        }
        Some(bits.trailing_zeros() as u8)
    }

    fn service_interrupt(&mut self, bit: u8) -> u32 {
        self.cpu.ime = false;
        self.iflag &= !(1 << bit);
        self.push16(self.cpu.r.pc);
        self.cpu.r.pc = 0x40 + (bit as u16) * 8;
        5
    }

    // ---- single-step ----------------------------------------------------------
    // Executes exactly one "unit of work" (one instruction, one interrupt
    // dispatch, or one idle M-cycle while halted) and advances the PPU/timer/
    // serial/RTC by the M-cycles it consumed. Returns the M-cycles consumed.
    pub fn step(&mut self) -> u32 {
        if self.cpu.ei_delay > 0 {
            self.cpu.ei_delay -= 1;
            if self.cpu.ei_delay == 0 {
                self.cpu.ime = true;
            }
        }

        if self.joyp.irq {
            self.joyp.irq = false;
            self.iflag |= 1 << 4;
        }
        if self.timer.irq {
            self.timer.irq = false;
            self.iflag |= 1 << 2;
        }
        if self.ppu.vblank_irq {
            self.ppu.vblank_irq = false;
            self.iflag |= 1 << 0;
        }
        if self.ppu.stat_irq {
            self.ppu.stat_irq = false;
            self.iflag |= 1 << 1;
        }
        if self.serial_shift_remaining > 0 {
            let consumed = self.serial_shift_remaining.min(4);
            self.serial_shift_remaining -= consumed;
            if self.serial_shift_remaining == 0 {
                self.sb = 0xFF; // nothing plugged in
                self.sc &= !0x80;
                self.iflag |= 1 << 3;
            }
        }

        let m_cycles: u32;
        if self.cpu.halted {
            if self.pending_interrupt().is_some() {
                self.cpu.halted = false;
                if self.cpu.ime {
                    let bit = self.pending_interrupt().unwrap();
                    m_cycles = self.service_interrupt(bit);
                } else {
                    m_cycles = 1;
                }
            } else {
                m_cycles = 1;
            }
        } else if self.cpu.ime {
            if let Some(bit) = self.pending_interrupt() {
                m_cycles = self.service_interrupt(bit);
            } else {
                m_cycles = self.execute_one();
            }
        } else {
            m_cycles = self.execute_one();
        }

        let t = m_cycles * 4;
        self.timer.step(t);
        self.ppu.step(t);
        self.apu.step(t);
        self.cart.tick_rtc(t);
        self.total_m_cycles += m_cycles as u64;
        m_cycles
    }

    pub fn run_frame(&mut self) -> u32 {
        let mut total = 0u32;
        self.ppu.frame_ready = false;
        // Safety cap well above one frame's worth of M-cycles (70224/4=17556)
        // so a runaway (e.g. an illegal-opcode lockup) cannot hang the app.
        while !self.ppu.frame_ready && total < 30000 {
            total += self.step();
        }
        total
    }

    fn execute_one(&mut self) -> u32 {
        let op = self.fetch8();
        if is_illegal(op) {
            // Real hardware locks up permanently on these. We stop advancing
            // PC (spin on the same illegal byte) rather than crash the app.
            self.cpu.r.pc = self.cpu.r.pc.wrapping_sub(1);
            return 1;
        }
        let (base, extra) = CYCLES_UNPREFIXED[op as usize];
        let mut cycles = base as u32;
        let x = op >> 6;
        let y = (op >> 3) & 7;
        let z = op & 7;
        let p = y >> 1;
        let q = y & 1;

        match op {
            0x00 => {}
            0x76 => {
                // HALT. The classic HALT-bug condition (Pan Docs): if IME=0
                // AND an interrupt is already pending, the CPU does not
                // actually halt; instead the byte after HALT is fetched twice.
                if !self.cpu.ime && self.pending_interrupt().is_some() {
                    self.cpu.halt_bug = true;
                } else {
                    self.cpu.halted = true;
                }
            }
            0x10 => {
                self.fetch8(); // STOP's second byte (always 0x00 in practice)
                self.cpu.stopped = true;
                self.timer.write_div();
            }
            0xF3 => self.cpu.ime = false,
            0xFB => self.cpu.ei_delay = 2,
            0x27 => self.daa(),
            0x2F => {
                self.cpu.r.a = !self.cpu.r.a;
                self.cpu.r.set_flag(FLAG_N, true);
                self.cpu.r.set_flag(FLAG_H, true);
            }
            0x37 => {
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
                self.cpu.r.set_flag(FLAG_C, true);
            }
            0x3F => {
                let c = self.cpu.r.flag(FLAG_C);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
                self.cpu.r.set_flag(FLAG_C, !c);
            }
            0x07 => {
                let v = self.rlc(self.cpu.r.a);
                self.cpu.r.a = v;
                self.cpu.r.set_flag(FLAG_Z, false);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
            }
            0x0F => {
                let v = self.rrc(self.cpu.r.a);
                self.cpu.r.a = v;
                self.cpu.r.set_flag(FLAG_Z, false);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
            }
            0x17 => {
                let v = self.rl(self.cpu.r.a);
                self.cpu.r.a = v;
                self.cpu.r.set_flag(FLAG_Z, false);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
            }
            0x1F => {
                let v = self.rr(self.cpu.r.a);
                self.cpu.r.a = v;
                self.cpu.r.set_flag(FLAG_Z, false);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, false);
            }
            0x08 => {
                let addr = self.fetch16();
                self.write16(addr, self.cpu.r.sp);
            }
            0xE8 => {
                let res = self.add_sp_e8();
                self.cpu.r.sp = res;
            }
            0xF8 => {
                let res = self.add_sp_e8();
                self.cpu.r.set_hl(res);
            }
            0xF9 => self.cpu.r.sp = self.cpu.r.hl(),
            0xE9 => self.cpu.r.pc = self.cpu.r.hl(),
            0xC3 => self.cpu.r.pc = self.fetch16(),
            0x18 => {
                let e = self.fetch8() as i8 as i16;
                self.cpu.r.pc = (self.cpu.r.pc as i16).wrapping_add(e) as u16;
            }
            0xCD => {
                let addr = self.fetch16();
                self.push16(self.cpu.r.pc);
                self.cpu.r.pc = addr;
            }
            0xC9 => self.cpu.r.pc = self.pop16(),
            0xD9 => {
                self.cpu.r.pc = self.pop16();
                self.cpu.ime = true;
            }
            0xE0 => {
                let n = self.fetch8();
                self.write8(0xFF00 + n as u16, self.cpu.r.a);
            }
            0xF0 => {
                let n = self.fetch8();
                self.cpu.r.a = self.read8(0xFF00 + n as u16);
            }
            0xE2 => self.write8(0xFF00 + self.cpu.r.c as u16, self.cpu.r.a),
            0xF2 => self.cpu.r.a = self.read8(0xFF00 + self.cpu.r.c as u16),
            0xEA => {
                let addr = self.fetch16();
                self.write8(addr, self.cpu.r.a);
            }
            0xFA => {
                let addr = self.fetch16();
                self.cpu.r.a = self.read8(addr);
            }
            0xCB => {
                let cbop = self.fetch8();
                cycles = CYCLES_CB[cbop as usize] as u32;
                self.exec_cb(cbop);
            }
            _ => match x {
                0 => match z {
                    0 => match y {
                        1 => {}     // handled as 0x08 above
                        2 => {}     // 0x10 STOP handled above
                        3 => {}     // 0x18 JR handled above
                        4..=7 => {
                            let e = self.fetch8() as i8 as i16;
                            if self.cc(y - 4) {
                                self.cpu.r.pc = (self.cpu.r.pc as i16).wrapping_add(e) as u16;
                                cycles += extra as u32;
                            }
                        }
                        _ => {}
                    },
                    1 => {
                        if q == 0 {
                            let v = self.fetch16();
                            self.set_rp(p, v);
                        } else {
                            let v = self.get_rp(p);
                            self.alu_add16_hl(v);
                        }
                    }
                    2 => {
                        let hl = self.cpu.r.hl();
                        if q == 0 {
                            let addr = match p {
                                0 => self.cpu.r.bc(),
                                1 => self.cpu.r.de(),
                                2 => {
                                    self.cpu.r.set_hl(hl.wrapping_add(1));
                                    hl
                                }
                                _ => {
                                    self.cpu.r.set_hl(hl.wrapping_sub(1));
                                    hl
                                }
                            };
                            self.write8(addr, self.cpu.r.a);
                        } else {
                            let addr = match p {
                                0 => self.cpu.r.bc(),
                                1 => self.cpu.r.de(),
                                2 => {
                                    self.cpu.r.set_hl(hl.wrapping_add(1));
                                    hl
                                }
                                _ => {
                                    self.cpu.r.set_hl(hl.wrapping_sub(1));
                                    hl
                                }
                            };
                            self.cpu.r.a = self.read8(addr);
                        }
                    }
                    3 => {
                        let v = self.get_rp(p);
                        if q == 0 {
                            self.set_rp(p, v.wrapping_add(1));
                        } else {
                            self.set_rp(p, v.wrapping_sub(1));
                        }
                    }
                    4 => {
                        let v = self.get_r(y);
                        let r = self.alu_inc(v);
                        self.set_r(y, r);
                    }
                    5 => {
                        let v = self.get_r(y);
                        let r = self.alu_dec(v);
                        self.set_r(y, r);
                    }
                    6 => {
                        let n = self.fetch8();
                        self.set_r(y, n);
                    }
                    _ => {}
                },
                1 => {
                    // LD r[y], r[z]; z==6,y==6 is HALT (handled above as 0x76)
                    let v = self.get_r(z);
                    self.set_r(y, v);
                }
                2 => {
                    let v = self.get_r(z);
                    self.alu_op(y, v);
                }
                _ => match z {
                    0 => match y {
                        0..=3 => {
                            if self.cc(y) {
                                self.cpu.r.pc = self.pop16();
                                cycles += extra as u32;
                            }
                        }
                        _ => {}
                    },
                    1 => {
                        if q == 0 {
                            let v = self.pop16();
                            match p {
                                0 => self.cpu.r.set_bc(v),
                                1 => self.cpu.r.set_de(v),
                                2 => self.cpu.r.set_hl(v),
                                _ => self.cpu.r.set_af(v),
                            }
                        }
                    }
                    2 => match y {
                        0..=3 => {
                            let addr = self.fetch16();
                            if self.cc(y) {
                                self.cpu.r.pc = addr;
                                cycles += extra as u32;
                            }
                        }
                        _ => {}
                    },
                    3 => {} // 0xC3 JP nn / 0xCB / 0xF3 DI / 0xFB EI handled above
                    4 => {
                        if y <= 3 {
                            let addr = self.fetch16();
                            if self.cc(y) {
                                self.push16(self.cpu.r.pc);
                                self.cpu.r.pc = addr;
                                cycles += extra as u32;
                            }
                        }
                    }
                    5 => {
                        if q == 0 {
                            let v = match p {
                                0 => self.cpu.r.bc(),
                                1 => self.cpu.r.de(),
                                2 => self.cpu.r.hl(),
                                _ => self.cpu.r.af(),
                            };
                            self.push16(v);
                        }
                    }
                    6 => {
                        let n = self.fetch8();
                        self.alu_op(y, n);
                    }
                    _ => {
                        self.push16(self.cpu.r.pc);
                        self.cpu.r.pc = (y as u16) * 8;
                    }
                },
            },
        }
        cycles
    }

    fn alu_op(&mut self, y: u8, v: u8) {
        match y {
            0 => self.alu_add(v, 0),
            1 => {
                let c = self.cpu.r.flag(FLAG_C) as u8;
                self.alu_add(v, c);
            }
            2 => {
                self.alu_sub(v, 0, true);
            }
            3 => {
                let c = self.cpu.r.flag(FLAG_C) as u8;
                self.alu_sub(v, c, true);
            }
            4 => self.alu_and(v),
            5 => self.alu_xor(v),
            6 => self.alu_or(v),
            _ => {
                self.alu_sub(v, 0, false);
            }
        }
    }

    fn exec_cb(&mut self, op: u8) {
        let x = op >> 6;
        let y = (op >> 3) & 7;
        let z = op & 7;
        let v = self.get_r(z);
        match x {
            0 => {
                let r = match y {
                    0 => self.rlc(v),
                    1 => self.rrc(v),
                    2 => self.rl(v),
                    3 => self.rr(v),
                    4 => self.sla(v),
                    5 => self.sra(v),
                    6 => self.swap(v),
                    _ => self.srl(v),
                };
                self.zflag_from(r);
                self.set_r(z, r);
            }
            1 => {
                let bit = (v >> y) & 1;
                self.cpu.r.set_flag(FLAG_Z, bit == 0);
                self.cpu.r.set_flag(FLAG_N, false);
                self.cpu.r.set_flag(FLAG_H, true);
            }
            2 => self.set_r(z, v & !(1 << y)),
            _ => self.set_r(z, v | (1 << y)),
        }
    }

    // ---- battery save --------------------------------------------------------
    pub fn has_battery(&self) -> bool {
        self.cart.has_battery
    }
    pub fn save_ram(&self) -> &[u8] {
        &self.cart.ram
    }
    pub fn load_ram(&mut self, data: &[u8]) {
        let n = data.len().min(self.cart.ram.len());
        self.cart.ram[..n].copy_from_slice(&data[..n]);
    }
}
