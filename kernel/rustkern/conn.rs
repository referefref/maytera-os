// rustkern/conn.rs - #487/#349 Task Manager accessor: per-process network connections
//
// Split out of the 9,566-line rustkern.rs (#404 / #526). PURE REFACTOR: the
// code below is carried across verbatim; the exported symbol list is unchanged
// and enforced by ../rust-symbols.manifest via tools/rust-symbol-gate.
//
// `#[no_mangle]` exports keep their exact C names regardless of the module they
// live in, so the FFI surface and every `extern` declaration on the C side are
// untouched.

// ===========================================================================
// #487/#349 Task Manager kernel accessor tier 3: PER-PROCESS NETWORK
// ATTRIBUTION (conn_filter_by_pid).
//
// WHY THIS EXISTS: tcp_conn_t carried no owning pid, so the Task Manager could
// only show a SYSTEM-WIDE connection table; "which process opened this socket"
// is a Process Explorer signature answer we could not give. tcp_conn_t now
// carries owner_pid (stamped in tcp_socket() from proc_current(); INHERITED
// from the listener on an inbound SYN, because that path runs from the RX IRQ
// where proc_current() is an unrelated victim). This seam selects one process's
// rows out of the snapshot.
//
// SAFETY VALUE (why Rust): the filter writes into a caller-sized array while
// iterating a different, larger one. The naive C is a textbook CWE-787: it
// increments the write index per match and only checks the INPUT bound, so a
// process with more connections than the caller's array can hold walks off the
// end. Here the destination is an exactly-out_cap slice, so the bound is a
// property of the type rather than of remembering to write the check.
//
// Routed live under -DRUST_CONN_OWNER (net/tcp.c keeps conn_filter_by_pid_c as
// the reference twin + rollback). Boot [RUST-DIFF] conn_owner proves rs == c.
// ===========================================================================

// Mirrors tcp_conn_info_t (net/tcp.h). sizeof-locked on both sides.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct TcpConnInfo {
    pub state: u8,
    pub is_listener: u8,
    pub local_port: u16,
    pub remote_port: u16,
    pub remote_ip: u32,
    pub recv_len: u16,
    pub send_len: u32,
    pub owner_pid: u32,
}
const _: () = assert!(core::mem::size_of::<TcpConnInfo>() == 24);

#[no_mangle]
pub extern "C" fn conn_filter_by_pid_rs(inp: *const TcpConnInfo, n: u32, pid: u32,
                                        out: *mut TcpConnInfo, out_cap: u32) -> i32 {
    if inp.is_null() || out.is_null() {
        return -1;
    }
    if out_cap == 0 || n == 0 {
        return 0;
    }
    // SAFETY: the caller guarantees `inp` spans >= n rows and `out` spans >=
    // out_cap rows (net/tcp.c passes its own arrays). Both are accessed ONLY
    // through these exactly-sized slices, so neither index can leave its
    // allocation: the CWE-787 the naive C filter has is unrepresentable here.
    let src: &[TcpConnInfo] = unsafe { core::slice::from_raw_parts(inp, n as usize) };
    let dst: &mut [TcpConnInfo] = unsafe { core::slice::from_raw_parts_mut(out, out_cap as usize) };
    let mut w = 0usize;
    for row in src.iter() {
        if row.owner_pid != pid {
            continue;
        }
        if w >= dst.len() {
            break; // caller's array is full; stop rather than over-write
        }
        dst[w] = *row;
        w += 1;
    }
    w as i32
}
