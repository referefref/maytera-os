// net/firewall.h - #238: the C face of the packet filter.
//
// THE FILTER ITSELF IS NOT HERE. Every decision, every rule, the connection
// tracker, the config parser/serialiser and the counters live in
// kernel/rustkern/fwfilter.rs, per the Rust-first rule. This header and its
// .c hold only what cannot be Rust:
//
//   * the `extern` declarations and the layout mirrors that C's
//     _Static_assert can lock (a Rust-side sizeof cannot fail a C build);
//   * the two hook wrappers, which exist so net/ip.c has one obvious call
//     rather than an FFI signature inline in a packet path;
//   * boot-time load and persistence, which are calls into the C-only
//     fat_read_file()/fat_write_file()/perms_set() APIs.
//
// No policy is expressed in C. That is the point of the split, and it is the
// justification required by CLAUDE.md for writing any new kernel C at all.
#ifndef NET_FIREWALL_H
#define NET_FIREWALL_H

#include "../types.h"

#define FW_MAX_RULES     12
#define FW_CFG_VERSION   1

// Direction
#define FW_DIR_IN        0
#define FW_DIR_OUT       1
// Action / default policy
#define FW_ACT_ALLOW     0
#define FW_ACT_DENY      1
// Rule protocol selector (NOT an IP protocol number)
#define FW_PROTO_TCP     0
#define FW_PROTO_UDP     1

// The one file. The KERNEL owns it: it is the only parser and the only
// writer. See fwfilter.rs for why userland does not get a second copy.
#define FW_CFG_PATH      "/CONFIG/FWRULES.CFG"

// Mirrors of the Rust #[repr(C)] types in rustkern/fwfilter.rs. The sizeof
// locks live in proc/syscall.c beside the other syscall-ABI asserts.
typedef struct {
    uint8_t  dir;        // FW_DIR_*
    uint8_t  action;     // FW_ACT_*
    uint8_t  proto;      // FW_PROTO_*
    uint8_t  reserved;   // must be 0
    uint16_t port;       // 1..65535
    uint16_t reserved2;  // must be 0
} fw_rule_t;

typedef struct {
    uint32_t  version;      // FW_CFG_VERSION
    uint8_t   enabled;      // 0/1
    uint8_t   pol_in;       // FW_ACT_*
    uint8_t   pol_out;      // FW_ACT_*
    uint8_t   rule_count;   // 0..FW_MAX_RULES
    fw_rule_t rules[FW_MAX_RULES];
} fw_config_t;

typedef struct {
    uint64_t calls;
    uint64_t ct_hit;
    uint64_t new_in, new_out;
    uint64_t pass_in, pass_out;
    uint64_t drop_in, drop_out;
    uint64_t exempt;
    uint64_t unfiltered;
    uint64_t malformed;
    uint64_t frag;
    uint64_t ct_evict;
    uint64_t cyc_tot, cyc_max;
    uint32_t ct_used;
    uint32_t enabled;
    uint32_t rule_count;
    uint32_t pol;           // pol_in | (pol_out << 8)
} fw_stats_t;

// The single transfer struct for SYS_NET_FW: ONE size for every op, so
// argtab.rs describes one fixed-size user write and there is no op-dependent
// pointer contract to get wrong.
typedef struct {
    fw_config_t cfg;
    fw_stats_t  stats;
} fw_xfer_t;

// ---- rustkern/fwfilter.rs -------------------------------------------------
// Returns 1 to pass the packet, 0 to drop it. NEVER blocks, allocates or
// calls out: it is reached from eth_receive()'s RX drain (net_lock held,
// interrupts OFF) and from ip_send() (reachable pre-scheduler, #549).
extern int  fw_filter_rs(uint32_t dir, uint32_t peer_ip, uint8_t proto,
                         uint16_t frag_off, const uint8_t *payload, uint16_t len);
extern int  fw_install_rs(const fw_config_t *cfg);       // 0 ok, -1 invalid (nothing changes)
extern int  fw_get_config_rs(fw_config_t *out);
extern int  fw_get_stats_rs(fw_stats_t *out);
extern void fw_reset_stats_rs(void);
extern int  fw_default_config_rs(fw_config_t *out);
extern int  fw_parse_cfg_rs(const uint8_t *text, uint32_t len, fw_config_t *out);
extern int  fw_format_cfg_rs(const fw_config_t *cfg, uint8_t *out, uint32_t max);
extern int  fw_selftest_rs(void);

// ---- net/firewall.c -------------------------------------------------------
// Run the boot self-test, then load FW_CFG_PATH and install it. Called once
// from net_init(), after the root filesystem is mounted. Any failure LEAVES
// THE COMPILED-IN POLICY IN FORCE; it never disables the filter.
void fw_boot_load(void);
// Serialise and write FW_CFG_PATH. Process context only (it takes fat_lock).
int  fw_persist(const fw_config_t *cfg);
// Re-read FW_CFG_PATH and install it. Returns the number of malformed lines
// ignored (>= 0), or negative on failure (previous policy retained).
int  fw_reload(void);
// One-line human summary onto the serial console / boot log.
void fw_report(const char *why);

// ---- the two hooks --------------------------------------------------------
// Both are `static inline` so net/ip.c gets a direct call with no wrapper
// frame on the packet path.

// Outbound. `dest_ip` is HOST byte order (what ip_send() is handed), `l4` is
// the transport header and body.
static inline int fw_check_out(uint32_t dest_ip, uint8_t proto,
                               const void *l4, uint16_t l4len) {
    return fw_filter_rs(FW_DIR_OUT, dest_ip, proto, 0,
                        (const uint8_t *)l4, l4len);
}

// Inbound. `src_ip` is HOST byte order, `frag_off` is the IPv4 fragment
// offset in 8-byte units (0 for a first or only fragment).
static inline int fw_check_in(uint32_t src_ip, uint8_t proto, uint16_t frag_off,
                              const void *l4, uint16_t l4len) {
    return fw_filter_rs(FW_DIR_IN, src_ip, proto, frag_off,
                        (const uint8_t *)l4, l4len);
}

#endif // NET_FIREWALL_H
