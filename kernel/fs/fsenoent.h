// fsenoent.h - #aicappersist: the C side of rustkern/fsenoent.rs.
//
// An ext2 ENOENT reaches Ring 3 as the single number -2, and -2 has three
// causes with three different fixes: the path never reached the kernel, the
// parent really is absent (so -2 is CORRECT), or the parent exists and the
// lookup missed it anyway (so -2 is a lie). Two agents reached opposite
// conclusions about which it was on the same image, from the number alone.
//
// The POLICY (what a fact set means, and whether it is worth a line) is Rust,
// per the standing directive. The GATHERING is C because the facts are
// file-static state inside fs/ext2.c, reachable only from there.
//
// NOTHING HERE CHANGES A DECISION. Every call site reports and then returns
// exactly the error it was already going to return. A refusal is never
// weakened to make a write succeed; the point is to learn which refusals are
// right.
#ifndef MAYTERA_FSENOENT_H
#define MAYTERA_FSENOENT_H

#include "../types.h"

// Call sites. Locked against the Rust constants by fsenoent_abi_check_rs().
#define FSE_SITE_PATHCOPY     1u   // sys_open_k: sc_path_from_user refused
#define FSE_SITE_PARENTGUARD  2u   // fdlayer: ext2_parent_dir_exists said no
#define FSE_SITE_NOCREATE     3u   // fdlayer: absent and no O_CREAT
#define FSE_SITE_READWHOLE    4u   // fat_read_file: the ext2 whole-file read missed

// Verdicts, in the order the classifier applies them (broadest cause first).
#define FSE_V_UNKNOWN            0u
#define FSE_V_NOT_MOUNTED        1u
#define FSE_V_ROOT_INO_LOST      2u
#define FSE_V_TRANSIENT          3u
#define FSE_V_FLAPPING           4u
#define FSE_V_PARENT_UNREADABLE  5u
#define FSE_V_PARENT_NOT_DIR     6u
#define FSE_V_CORRECT_ABSENT     7u
#define FSE_V_PATHCOPY           8u

// Mirror of FsEnoentFacts in rustkern/fsenoent.rs.
typedef struct {
    uint32_t site;
    uint32_t mounted;        // g_ext2.mounted, read at report time
    uint32_t root_ext2;      // g_root_ext2
    uint32_t root_ino;       // what "/" resolves to NOW (2 when healthy)
    uint32_t parent_ino_1;   // first re-resolve of the parent, AFTER the refusal
    uint32_t parent_ino_2;   // second re-resolve, to catch a flapping lookup
    uint32_t parent_mode;    // parent i_mode when readable, else 0
    int32_t  inode_rc;       // ext2_read_inode rc for the parent
    uint32_t guard_said;     // what the refusing guard returned
    uint32_t uid;            // effective uid of the refused caller
} fsenoent_facts_t;

_Static_assert(sizeof(fsenoent_facts_t) == 40,
               "fsenoent_facts_t must stay layout-identical to the Rust FsEnoentFacts");

// Rust (rustkern/fsenoent.rs).
uint32_t fsenoent_classify_rs(const fsenoent_facts_t *f);
int32_t  fsenoent_should_log_rs(uint32_t site, uint32_t verdict);
int32_t  fsenoent_abi_check_rs(uint32_t facts_size,
                               uint32_t site_pathcopy, uint32_t site_parentguard,
                               uint32_t site_nocreate, uint32_t site_readwhole,
                               uint32_t v_not_mounted, uint32_t v_transient,
                               uint32_t v_correct_absent, uint32_t v_pathcopy);
int32_t  fsenoent_selftest_rs(uint32_t *out_checks);

// C (fs/ext2.c). Gathers the facts for `path` (an absolute ext2-volume path)
// and, when the verdict is worth a line, prints ONE line naming the verdict and
// every fact behind it. `path` may be NULL for FSE_SITE_PATHCOPY.
void fsenoent_report(uint32_t site, const char *path, uint32_t guard_said);

// Boot wiring (fs/ext2.c): runs the ABI lock and the self-test and logs both.
void fsenoent_boot_selftest(void);

#endif // MAYTERA_FSENOENT_H
