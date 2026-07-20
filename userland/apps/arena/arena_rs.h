/* arena_rs.h - C <-> Rust (userland, Ring-3) FFI surface for Maytera Arena.
 *
 * Task #491: Rust code linked into the Arena userland ELF. Keep this surface
 * SMALL and repr(C)/integer only. See arena_rs.rs for the implementation and
 * ARENA_BSP_PLAN.md for the roadmap (Stage 1 adds the GoldSrc BSP v30 parser).
 */
#ifndef ARENA_RS_H
#define ARENA_RS_H

#include <stdint.h>

/* Stage 0 smoke test: proves no_std + alloc::Vec + FFI + bounds checking run
 * in Ring-3. Returns the deterministic magic 0xA55AE73B (low byte 0x3B == the
 * Vec sum 59), so the caller can confirm the Rust object actually executed.
 * Safe to call any number of times; touches only its stack + the shared heap. */
uint32_t arena_rs_selftest(void);

#define ARENA_RS_SELFTEST_MAGIC 0xA55AE73Bu

#endif /* ARENA_RS_H */
