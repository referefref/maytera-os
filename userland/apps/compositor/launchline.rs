// launchline.rs - THE Rust definition of "which part of a Start-menu launch
// line is the BINARY" (#172 made the argument possible, #220 fixed the reader
// that never learned about it).
//
// A Start-menu item's second field is a LAUNCH LINE, not a bare path:
//
//     item: Stunts | /DOS/STUNTS/LOAD.EXE /u MCGA | icon=dosapp | type=dos
//
// #172 made that legal by teaching the KERNEL to split it:
// kernel/dos/dosexec.c::dos_launch_common() copies bytes into g_dos_path until
// the first ' ' or '\t' and puts the remainder in g_dos_cmdtail. That is the
// authoritative rule, and its own comment says why it lives in one place:
// "two copies of this rule is how the DOSRUN.CFG path and the syscall path
// came to disagree in the first place."
//
// The compositor then acquired a SECOND reader of the same field and no third
// copy of the rule. sm_model_finish()'s existence check handed the WHOLE field
// to access(), so the Stunts entry was stat()ing the literal string
// "/DOS/STUNTS/LOAD.EXE /u MCGA", which is not a file, so the entry was
// silently dropped and the game was unreachable from the Start menu even
// though /DOS/STUNTS/LOAD.EXE shipped on the ext2 root.
//
// MEASURED on golden build 2010 (commit abff9883), VM boot, keyboard-driven:
// typing "stun" into the Start-menu search returned "No matches" while "skyr"
// matched SkyRoads - the same fragment, the same category, the same file
// 03-games.MENU, differing only in that SkyRoads' launch line carries no
// argument. That A/B is the whole bug.
//
// THE ONE PLACE THE WHOLE LINE MUST STILL TRAVEL INTACT IS THE LAUNCH.
// startmenu.c hands the full field to dos_run()/win16_run()/sys_spawn and the
// kernel splits it again on its own side. Splitting for the existence check
// must never turn into splitting for the launch, or every argument would be
// lost and Stunts would go back to printing "Invalid file name." and exiting.
//
// ONE DEFINITION, NOT A COPY. This file is include!()d by startmenu_model.rs,
// so the compositor compiles exactly one instance of the rule, and it is
// include!()d AGAIN by tools/launchline/conformance.sh's host harness, so the
// conformance table is checked against this exact source text rather than
// against a re-implementation that could only ever prove itself.

/// The BINARY part of a launch line: leading ASCII whitespace skipped, then
/// everything up to (not including) the next ASCII whitespace byte. Never
/// allocates; the result borrows `line`.
///
/// The leading-whitespace skip and the CR/LF terminators are a deliberate
/// SUPERSET of what dos_launch_common() does. The kernel never sees an
/// untrimmed line (every producer trims first), so on the inputs that actually
/// occur the two agree exactly; accepting the untrimmed forms here means the
/// shell definition in build/invariant-gate.sh, which reads raw CRLF fragment
/// text off an image, can share the same conformance table instead of needing
/// its own dialect.
///
/// Splitting on ASCII whitespace only ever stops on a single-byte character,
/// so the returned slice is always on a UTF-8 boundary.
pub fn launch_binary(line: &str) -> &str {
    let b = line.as_bytes();
    let is_ws = |c: u8| c == b' ' || c == b'\t' || c == b'\r' || c == b'\n';
    let mut start = 0usize;
    while start < b.len() && is_ws(b[start]) {
        start += 1;
    }
    let mut end = start;
    while end < b.len() && !is_ws(b[end]) {
        end += 1;
    }
    &line[start..end]
}
