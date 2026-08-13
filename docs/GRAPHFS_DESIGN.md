# GraphFS: the storage substrate for contract-based isolation (#711)

Status: DESIGN, plus TWO implemented and VM-VERIFIED vertical slices.
Slice 1 is the tamper-evident journal (section 18: four tamper detections proven
RED from Ring 3). Slice 2 is the FOLD (section 19): nodes, edges, contracts,
Q1-Q3, replay at boot, and a boot enforcement self-test that is itself proven RED
by deleting the escrow rule and watching it get caught by name.
Read `docs/CONTRACT_ARCHITECTURE.md` first; every section here exists to satisfy a
numbered requirement in that document, and says which one.

Everything below is marked MEASURED (I ran it and read the output) or INFERRED
(reasoning from measured facts). Prose in this tree has lied before; the
measurements are reproducible from the commands in section 14.

---

## 1. The one-paragraph version

GraphFS is not a filesystem for user files. ext2 is that, and it works. GraphFS is
the **append-only, content-addressed, tamper-evident store that the contract layer
runs on**: contracts are edges in a versioned graph, the audit journal is the
ordered log of every mutation to that graph, and the graph state is nothing more
than a fold of the journal. That single sentence is the whole design, and every
decision below falls out of it.

The name is inherited. If it were being named today it would be called the
contract journal, because the journal is the load-bearing part and the graph is
the shape the journal's fold happens to take.

---

## 2. What is actually there today (MEASURED, 2026-08-06, commit a36311c)

| claim | measurement | command |
|---|---|---|
| 72 `gfs_*` functions declared | 72 unique, node.h 29 + version.h 20 + query.h 23 | `grep -hoE "gfs_[a-z_0-9]+\(" node.h version.h query.h \| sort -u \| wc -l` |
| zero implemented | zero `gfs_` occurrences anywhere outside those three headers, tree-wide | `grep -rn "gfs_" --include=*.c --include=*.rs .` |
| zero callers | same grep; `blob.c`/`blob_test.c` contain zero `gfs_` | as above |
| `blob.c` is real | 28 `blob_*` functions implemented, SHA-256 content-addressed, refcounted | reading blob.c |
| `blob.c` has never executed | `blob_store_init`, `blob_store_self_test` have zero callers tree-wide | `grep -rn "blob_store_init\|blob_store_self_test"` |
| `sizeof(gfs_node_t)` | **3,832 bytes** | binary search with `_Static_assert(sizeof(gfs_node_t) <= N)` under kernel CFLAGS |
| `GFS_MAX_NODES` cost | 65536 x 3832 = **239.5 MiB** of the 256 MB kernel heap, for an EMPTY graph | arithmetic on the measured size |
| `sizeof(gfs_query_t)` | 5,192 bytes | same method |
| the API's floats are unsound here | a `float` argument compiles, but GCC marshals it through the **x87 FPU** (`flds`/`fstps`) because SSE is disabled; a `float` *return* is a hard error: `SSE register return with SSE disabled` | `gcc -mno-sse -mno-sse2 ... -S` on a call to `gfs_edge_create` |
| there is no wall clock | `sys_time()` returns `timer_ticks / g_timer_hz` = **seconds since boot**; no CMOS/RTC reader exists in `drivers/` or `cpu/` | reading `proc/syscall.c:4661`, grep for RTC |
| `kernel/rust/` is dead scaffolding | 80 lines of cargo stubs; `rust_blob_store` is `// TODO` + `return -1`; not referenced by the Makefile, which builds `rustkern.rs` only | reading `kernel/rust/src/*.rs`, `kernel/Makefile` |

Two things follow immediately.

**The 72 declarations contain no journal, no append-only anything, no tamper
detection, and no contract concept.** Their centre of gravity is semantic search
for LLM file discovery: tags, descriptions, embeddings, natural-language path
resolution, relevance scores. That is a different product. It is why a graph
filesystem designed in the abstract could not be retrofitted to the contract
model: the thing the contract model most needs is the thing the prior API most
completely lacks.

**The x87 finding is worse than "floats are discouraged".** `-mno-sse` does not
stop the compiler emitting float code; it makes it fall back to x87 and push the
value on the stack. This kernel never saves x87/SSE state across a context switch
(`sse_save`/`sse_restore` have zero callers, CLAUDE.md), so a float in a
preemptible kernel path silently corrupts, or is corrupted by, any other x87 user.
`gfs_edge_t.weight`, `gfs_embedding_t.dims[128]`, `gfs_query_result_item_t.relevance`,
`gfs_nlp_response_t.confidence` and `gfs_search_similar(..., float threshold, ...)`
are all in that class.

---

## 3. Requirements, traced

Each row is a requirement from `docs/CONTRACT_ARCHITECTURE.md` and the mechanism
here that serves it. Anything in this design that serves no row should be deleted;
anything in a row with no mechanism is a gap and is named as one.

| req | what it demands | mechanism here | section |
|---|---|---|---|
| §4 | contracts explicit, time-limited, scoped, directional, revocable, auditable | a contract IS a directed GRANT edge with scope + expiry; revocation is an appended REVOKE record; the journal makes it auditable by construction | 5, 6 |
| §5 | escrow is the only issuer | GraphFS stores and answers; it never decides. The only writer of GRANT records is the escrow principal, enforced at the append chokepoint | 8 |
| §6 | enforcement at the syscall chokepoint; revocation bites in flight | the enforcement query is answered from the in-memory fold, updated by REVOKE before the append returns, so the next check after a revoke denies | 7 |
| §7 | rollback needs effects to be undoable: journal plus versioned state | every mutation is a journal record carrying its effect class (reversible / compensatable / irreversible), its actor identity, and the pre-image content hash; "revert task T" is a reverse walk of T's records | 6, 7 |
| §7 binding rule | the undo surface must never lie | effect class is declared in the record BEFORE execution, so an irreversible effect is enumerable at revert time and can be named | 6 |
| §12.6 | tamper-evident storage: append-only, detect truncation and reordering, not only edits | fixed-size records in a SHA-256 hash chain, plus a sealed head, plus an in-kernel head hash | 9 |
| §12.7 | fail closed | an unreadable, ragged or chain-broken journal marks the store DEGRADED and denies contract issuance; it never reports success | 9.4 |
| §12.9 | no bypass path | state is a fold of the journal and there is exactly one append function; there is no "write the node table" API to forget about. The on-disk files are read-only evidence, not a second write path | 8, 10 |
| §10 anti-pattern | unlimited contracts must be visibly expensive | `gfs_stats` counts unlimited-scope and unlimited-expiry grants as first-class counters, so the number can be put where it is seen | 7 |
| §3 | identity is plural; AI identity is per task | an identity is a node; a task identity is a node with a lifetime; contracts are edges from it. "Revert this task" = reverse-walk records whose actor is that node | 5 |

---

## 4. What GraphFS is not

Stated explicitly, because scope creep here is what produced 72 orphan declarations.

- **Not a POSIX namespace.** It does not mount, does not serve `open`/`read`, and
  does not own user files. ext2 does that.
- **Not a search engine.** No tags, no descriptions, no embeddings, no relevance,
  no natural-language resolution. Those are Ring-3 features if they are features at
  all, and three of them cannot be expressed in this kernel's float ABI anyway.
- **Not a git.** No branches, no merges, no text diffing in Ring 0.
- **Not a general database.** Six named queries (section 7), not a filter engine.

---

## 5. The model

### 5.1 Objects (immutable, content-addressed)

Bytes, keyed by SHA-256. This is `blob.c` and it already exists; see section 11.
Nothing else in the design stores bytes.

### 5.2 Nodes (identities and managed objects)

A node is an identity that persists across changes to its state:

```
node := { node_id: u64, kind: u16, born_seq: u64, state: u16 }
```

`kind` is one of: APP, SERVICE, USER, DEVICE, AI_TASK, OBJECT, CONTRACT_DEF.
The first six are §3's identities; OBJECT is a thing under contract management (a
file, a settings key, a device capability set); CONTRACT_DEF is an escrow-held
contract definition (§5).


**Node ids are `(kind << 24) | local`** (added in slice 2). The kind is therefore
DERIVABLE from the id, which buys two things. The fold cross-checks the two on
every `NODE_CREATE` and refuses a record whose id lies about its own kind, which
is a free integrity check. And a producer that can compute a stable `local` (a
uid, an AI task counter, a device index) can RECOMPUTE the node id later without
the graph ever storing a name, which is what lets 5.2's "a node carries no path
and no description" survive contact with the need to look one up.

A node carries no path, no description, no tags, no mime type, no embedding, and
no edge array. It is 32 bytes, not 3,832. Everything else about it is either a
version record (state over time) or an edge (relationships), both of which live in
the journal, which is the only thing that is allowed to grow.

### 5.3 Edges (relationships, and therefore contracts)

```
edge := { edge_id: u64, from: u64, to: u64, rel: u16,
          scope_id: u32, expiry: u64, granted_seq: u64, revoked_seq: u64 }
```

`rel` is a small enum (`GRANT`, `NAME`, `DERIVED_FROM`, `CONFIG_FOR`, ...), not a
32-byte string, so relation comparison is an integer compare on the enforcement
hot path.

**A contract is an edge with `rel = GRANT`.** Look at §4's four required properties
against that struct: directional is `from`->`to` and only that direction;
scope-limited is `scope_id`; time-limited is `expiry`; explicit is the fact that
the edge had to be appended by the escrow and can be listed. Revocable is
`revoked_seq != 0`, written by a REVOKE record. The contract manager UI in §8 of
the architecture is exactly `gfs_edges_get_outgoing(holder)` and
`gfs_edges_get_incoming(target)`.


### 5.3.1 What slice 2 changed about that struct, and why (MEASURED)

The struct above is 46 bytes. A journal record carries **16 inline bytes**
(section 6.2); anything longer must live in the content-addressed blob store,
which has seven measured defects and has never executed (section 11). Making the
fold depend on it would make "the graph works" contingent on "the blob store
works", which is exactly the ordering slice 1 refused.

The alternative was a format v2 with a wider inline field. That is worse, and the
reason is the same one that makes the journal worth having: a v2 record size
would make every existing chain a `BAD_VERSION`, i.e. a **false tamper alarm** on
every installed machine, and migrating the old records into the new size means
REWRITING HISTORY, which is the one thing an append-only log may never do.

So the payload was made to fit, and every byte saved was saved by DERIVING it
from something the record already carries rather than by dropping a property:

| field | before | slice 2 | why this is not a loss |
|---|---|---|---|
| `edge_id` | u64 in the payload | **gone**: it IS the record's own `seq` | an edge's identity becomes its position in the tamper-evident chain, so it cannot be forged, duplicated, or disagreed about by two readers |
| `granted_seq` | u64 in the payload | **gone**: same field | as above |
| `revoked_seq` | u64 in the payload | **gone**: the fold writes it when it sees the `EDGE_REVOKE` | it was never something the ADD record could know |
| `from`, `to` | u64 | u32 | 16.7M identities per kind domain; the in-memory table caps far below that |
| `scope_id` | u32 | u16 | scope is "a named capability" (architecture section 4). 65,535 named capabilities is not a constraint anyone will meet |
| `expiry` | u64 absolute | u32 ms of THIS boot's uptime | section 6.1 already says a contract cannot outlive a boot without a trusted clock, so an absolute 64-bit expiry was storing precision the system cannot honour. 49.7 days of uptime |

Total: exactly 16 bytes. Two further changes are policy, not packing:

* **`scope == 0` and `expiry == 0` are INVALID, not "unlimited".** A zeroed
  payload must be incapable of producing authority. The unlimited forms are the
  explicit sentinels `SCOPE_ANY` and `EXPIRY_NEVER`, and both are COUNTED, which
  is section 10's "make the wrong thing visibly expensive" made checkable.
* **The escrow rule is enforced in the FOLD, not only at issuance**, so it
  applies to the replay path too: a `GRANT` record whose actor is not the escrow
  node does not become authority even if it somehow reached the log.

This is the reason the graph shape is worth having at all. If contracts were rows
in a table, "why can this app reach that file" would be unanswerable; as edges,
it is a bounded walk (section 7, query Q6).

### 5.4 Versions (state over time)

A version record is an immutable snapshot of one node's state:

```
version := { node_id, version_no, content_hash: [u8;32],
             prev_version_hash: [u8;32], actor, effect_class, seq }
```

**What versioning means here, precisely:** a node's state is never overwritten. A
change appends a version record whose `prev_version_hash` is the SHA-256 of the
previous version record. So a node's history is itself a hash chain, and reverting
(§7) means appending a new version whose `content_hash` equals an older one's.
History is never rewritten, which is what makes "undo" and "audit" the same
mechanism instead of two mechanisms that can disagree.

### 5.5 The journal is the only writer

Node creation, version commit, edge add, edge revoke, node retire and node restore
are **not six APIs**. They are six record types in one append-only journal. The
in-memory node table, edge table and version index are a **fold** of that journal,
rebuilt at boot by replaying it.

That is the §12.9 property, obtained by construction rather than by discipline:
there is no second way to change the state, because there is no code that changes
the state. There is only code that appends a record and code that folds records.

---

## 6. The journal record

Fixed size, 160 bytes, little-endian, `#[repr(C)]`, `_Static_assert`-locked on the
C side.

```
off  size  field
  0   4    magic        'GFJ1'
  4   2    version      1
  6   2    rec_len      160
  8   8    seq          record index, 0-based, contiguous, no gaps
 16   8    boot_gen     which boot wrote this (persisted, monotonic)
 24   8    mono_ms      milliseconds since THAT boot (see 6.1)
 32   4    actor_kind   0 kernel, 1 pid, 2 node id (task/app identity)
 36   4    actor_id
 40   2    op           NODE_CREATE, VERSION_COMMIT, EDGE_ADD, EDGE_REVOKE,
                        NODE_RETIRE, NODE_RESTORE, AUDIT, CHECKPOINT
 42   2    effect       0 reversible, 1 compensatable, 2 irreversible, 3 n/a
 44   4    payload_len  length of the payload the hash below covers
 48  32    payload_hash SHA-256 of the payload (its blob key when stored)
 80  32    prev_hash    SHA-256 of the whole previous record (zero for seq 0)
112  16    inline       small payloads live here verbatim (see 6.2)
128  32    self_hash    SHA-256 over bytes [0..128) of THIS record
```

Fixed size is a deliberate constraint, not laziness: it makes the chain walkable
backwards, makes truncation arithmetic exact (`size % 160 != 0` is a ragged file,
detectable on its own), and makes a Ring-3 verifier trivial to write, which matters
because the audit trail should be checkable by anything, not only by the code that
wrote it.

### 6.1 Time, honestly

**MEASURED: this kernel has no wall clock.** `sys_time()` returns seconds since
boot; there is no RTC reader. `blob.c` "solves" this with a fake incrementing epoch
starting at Nov 2023 (`blob.c:101-106`, already recorded in `FAKE.md`). A fabricated
timestamp in an audit record is worse than an absent one, because it looks like
evidence.

So records carry `(boot_gen, seq, mono_ms)` and **no wall-clock field at all**.
Ordering is by `(boot_gen, seq)`, which is total, monotonic and verifiable.
`mono_ms` is uptime, labelled as uptime, and is additionally untrustworthy as a
duration under KVM (recorded in memory: the tick counter replays lost ticks in
bursts under vCPU starvation).

Two consequences that the contract layer must inherit rather than paper over:

1. **§4's "time-limited" cannot be honoured across a reboot until there is a
   trusted clock.** Therefore contracts do not survive a reboot: at `gfs_init` the
   fold marks every GRANT edge from a previous `boot_gen` as expired. That is
   fail-closed (§12.7) and it is a real limitation, not a workaround dressed up as
   a feature. Getting real expiry needs an RTC read plus a monotonic anchor; that
   is a separate prerequisite ticket, and it should be filed before the contract
   layer ships, not after.
2. `boot_gen` is itself useful: a journal whose last record claims a `boot_gen`
   higher than the seal's is evidence of a rollback attempt.

### 6.2 Payloads

Records are fixed-size, so anything longer than the 16 inline bytes is stored as a
blob and referenced by `payload_hash`. Content addressing is `blob.c`'s job and is
not reimplemented (section 11). The inline field exists so that the overwhelmingly
common small record (an audit event code, an edge id, a version number) costs zero
extra I/O, and so that slice 1 does not have to depend on blob.c before blob.c has
ever executed.

---

## 7. The query surface

Six questions. If a proposed query is not on this list, the contract layer does not
need it and it does not go in the kernel.

| id | question | consumer | constraint |
|---|---|---|---|
| Q1 | does holder H hold a live GRANT to target T with scope S, right now? | syscall enforcement chokepoint (§6) | **hot path**: no allocation, no I/O, no blocking, bounded time. Answered from the in-memory fold only |
| Q2 | what contracts does H hold? | contract manager (§8) | bounded out-array supplied by the caller |
| Q3 | what contracts exist on T? | contract manager, consent surface | as Q2 |
| Q4 | what did identity X do? | audit view, and the input to "revert task X" (§7) | journal scan by actor, blockable context only |
| Q5 | what was node N's state at journal position P? | rollback engine (§7) | version chain walk |
| Q6 | by what chain of grants can H reach T? | "why does this app have access" | bounded-depth walk over GRANT edges only, no free text |

Plus two integrity operations that are not queries but belong to the same surface:
`gfs_verify` (section 9) and `gfs_stats` (which must count unlimited-scope and
unlimited-expiry grants, per §10's "make the wrong thing visibly expensive").

**Q1's constraints drive the whole storage split.** The enforcement check can be
reached from contexts that must not block, so it must be answerable from memory.
Durability is therefore asynchronous for AUDIT records (the existing `seclog`
pattern: non-blocking producer, wait-queue wake, one blockable worker doing all
I/O), but **synchronous for GRANT records**: an authority that exists in memory
with no durable record of its issuance is unauditable, which §4 forbids. Revokes
take effect in memory before the append returns, satisfying §6's "revocation bites
in flight", and a lost revoke record is safe because contracts do not survive a
reboot anyway (6.1).

---

## 8. Where the chokepoint is

One function appends. Everything else reads.

```
gfsj_append(actor, op, effect, payload) -> seq | error
```

Enforcement of *who may append what* lives at that one call:

- `EDGE_ADD` with `rel = GRANT` is refused unless the caller is the escrow
  principal (§5: an app that can grant itself a contract is theatre, §14 open
  decision 2).
- `AUDIT` may be appended by kernel subsystems only.
- Ring 3 has **no append syscall at all** in this design. The Ring-3 surface is
  read-only: verify, list, history. Every mutation reaches the journal because a
  kernel subsystem decided it should, never because a userland process asked.

This is the §12.9 discipline applied in advance: the way to not have a second,
forgotten path to a protected object is to not build the first optional one.

---

## 9. Tamper evidence, and its honest boundary

§12.6 demands append-only storage with detection of **truncation and reordering,
not only edits**. Three independent mechanisms, because each catches what the
others miss.

### 9.1 The chain (catches edits and reordering)

`record[n].prev_hash == SHA256(record[n-1])`, and `record[n].self_hash ==
SHA256(record[n][0..128))`. Editing any byte of any record breaks that record's
`self_hash` and every subsequent `prev_hash`. Swapping two records breaks both the
chain and the `seq` contiguity check. Verification reports the **first** bad seq and
which check failed, so the report is diagnostic and not just "invalid".

### 9.2 The seal (catches truncation)

A truncated chain is still internally consistent, so the chain alone cannot detect
it. `/GRAPHFS/JOURNAL.SEAL` holds `{count, boot_gen, head_hash}` and is rewritten
after each append. `count` less than the seal's, or a `head_hash` mismatch, is
truncation. `size % 160 != 0` catches a partial (ragged) tail write separately, and
is reported as its own reason so a crash mid-append is distinguishable from an
attack.

### 9.3 The in-kernel head (catches an ADAPTIVE attacker, within a boot)

The chain and the seal are both unkeyed files. An attacker with write access to
both can rewrite history consistently and neither 9.1 nor 9.2 will notice. This is
the honest boundary, and it is exactly the point at which most "tamper-evident"
claims quietly stop.

So the kernel also keeps `{count, head_hash}` **in kernel memory**, updated on
every append. `gfs_verify` compares the on-disk chain against that in-memory head.
Ring 3 cannot write kernel memory, so **within a boot, any Ring-3 tamper is
detected even if the attacker recomputes the entire chain and the seal.** That is a
real property and it is proven RED in section 13, test 4.

Across a reboot the in-memory anchor is gone and detection degrades to 9.1 + 9.2,
i.e. to non-adaptive tampering only. Closing that needs the seal to be **signed by
a key the attacker cannot read**, which means the key lives in the immutable
security core (#305) and the file lives where a non-root session cannot write it
(#679). Both are already prerequisites of the contract architecture (§13 build
order), and this design does not pretend to have done their job. What it does is
put the verification surface in place now, so that adding the signature later is a
change to one function and not a change to the format.

**What this does NOT defend against, stated plainly:**
- an attacker with kernel memory write. Nothing here helps; that is game over.
- an offline attacker with the disk and no running kernel, before the seal is
  signed. Detected only if they are careless (9.1/9.2).
- an attacker who deletes the journal entirely. Detected as "absent", which
  fails closed (9.4), but the content is gone; append-only is not backup.

### 9.4 Fail closed (§12.7)

Every failure mode of `gfs_init` (file absent when the seal says it should exist,
ragged, chain broken, head mismatch) puts the store in **DEGRADED** state, which:
loudly logs the reason and the offending seq on serial; refuses to issue or honour
GRANT records; and keeps appending (so the tamper itself is recorded, after a
`CHECKPOINT` record marking the discontinuity). It never silently continues and it
never reports success. #622 shipped the opposite bug and it is not repeating here.

---

## 10. On-disk layout and VFS exposure

```
/GRAPHFS/JOURNAL.LOG    append-only chain of 160-byte records
/GRAPHFS/JOURNAL.SEAL   96-byte sealed head {count, boot_gen, head_hash}
/GRAPHFS/BLOBS/xx/<hex> content-addressed payloads (blob.c, section 11)
```

**On the ext2 root, never on the FAT ESP.** This is not a preference:
`build/invariant-gate.sh` fails any image whose p1 FAT holds anything but boot
assets, so a blob store on the ESP would make the golden unbuildable. It is also
correct on its own terms (the ESP is 256 MB and is the firmware's).
`fat_write_file`/`fat_read_file` route "/" paths to ext2 when `g_root_ext2` is set
(`fs/fat.c:180`), and `/boot` and `/EFI` are explicitly excluded from that routing,
so using the ordinary file API gets ext2 automatically. **If the root is not ext2,
`gfs_init` refuses to initialise** rather than quietly writing to the ESP.

Appends use `ext2_append_file` (`fs/ext2.c:2930`), which is a real append. The
`seclog` audit log by contrast rewrites its whole file every drain
(`fat_write_file(SECLOG_PATH, g_log, g_log_len)`), which is why `SECURITY.LOG`
cannot be append-only in any meaningful sense and is one reason this journal exists
beside it rather than inside it.

### VFS exposure: read-only evidence, one write path

The journal files are ordinary ext2 files, so anything can read them: the syslog
app, a userland verifier, a support bundle. That is deliberate. An audit trail
nobody can inspect is not an audit trail (§4: "a contract nobody can see or
withdraw is not a contract").

They are **not** a write interface, and GraphFS is **not** exposed as a mountable
namespace where `write()` on a path mutates the graph. A pseudo-filesystem would be
precisely the "second route to a protected object" that §12.9 forbids, and path
semantics cannot express "append a record with a declared effect class and an actor
identity" anyway. The API is typed kernel calls; the Ring-3 surface is
`SYS_GFS_VERIFY` (read-only) plus, later, read-only list/history calls.

---

## 11. How this sits on blob.c

Reuse, do not reimplement. `blob.c` is the only content-addressing primitive and
the design adds none: `blob_hash_compute`, `blob_store`, `blob_load`, `blob_ref`,
`blob_unref`, `blob_gc` stay as they are conceptually. The journal stores
`payload_hash` and the blob store stores the bytes.

But it is a dependency that has **never executed** (MEASURED), so before the graph
layer depends on it, these must be fixed. They are listed here as work, not as
excuses:

| # | defect (measured by reading blob.c) | why it matters |
|---|---|---|
| B1 | `get_current_time()` returns a fake incrementing epoch (line 101) | a fabricated timestamp in an audit-adjacent store; already in `FAKE.md` |
| B2 | `blob_store_format()` creates 256 directories on first init | 256 ext2 mkdirs at boot; never measured because it has never run |
| B3 | no locking anywhere; `blob_ref` is read-meta / increment / write-meta | lost refcount updates under concurrency; this tree already lost a filesystem to a shared-buffer RMW race (b103) |
| B4 | refcount lives in a second `.meta` file per blob | two files and two writes per ref; the index and the metas can disagree with no way to tell which is right |
| B5 | `blob_store(size=0)` returns a hash without storing anything, but `blob_load` of it fails | asymmetric API; an empty file is a legal file |
| B6 | `blob_store_init` takes `fat_fs_t*` and a base path with no ext2/ESP guard | on a FAT-root image it writes to the ESP and breaks the invariant gate |
| B7 | statistics are updated non-atomically and only persisted in `shutdown` | an unclean shutdown loses the index; `blob_store_shutdown` already says so out loud |

Slice 1 deliberately does **not** depend on any of that: its records are
self-contained (payload in the 16 inline bytes, `payload_hash` computed over it),
so the journal is provably working before the blob store's first execution. That
ordering is the point. See section 15.

---

## 12. Rust, and the C that is left

Standing rule: new kernel code is Rust. Applied here as:

**Rust (`rustkern/gfsjournal.rs`)**: the record format, encode, `self_hash`
computation, the chain walk, all verification logic, the seal format and its
checks. Every byte-level decision and every bounds check is in the memory-safe
language, which is the part where a hostile input (a tampered journal) is parsed.
That is the right split on the merits, not just by policy: verification is exactly
the code an attacker gets to feed.

**C (`fs/graphfs/journal.c`), and why it is not Rust**: the I/O and threading glue,
about 200 lines. `wait_event` is a C macro over `__wait_prepare`, `proc_create_ex`
takes a C function pointer, and `fat_read_file`/`ext2_append_file` are C with no
Rust bindings in this tree. Writing that glue in Rust would mean inventing a Rust
binding layer for the filesystem and wait-queue APIs, which is a large seam of its
own and would be the first thing to break when either API changes. The glue carries
no format knowledge and no parsing: it reads a file into a buffer, hands it to
Rust, and writes back what Rust produced. **This is a stated justification per the
CHANGELOG rule, not "the surrounding code is C".**

FFI is `#[repr(C)]` with `_Static_assert` size locks on the C side, matching the
established pattern in `RUST_PORT_LEDGER.md`. Exports are declared in
`rust-symbols.manifest` so the symbol gate fails the build if one disappears.

No floats anywhere, for the reason measured in section 2. No `std`; `core` only
(the slice needs no allocation at all: it works in caller-provided buffers).

---

## 13. Concurrency, and the no-busy-wait rule

- The journal worker sleeps on a **wait queue** (`sync/waitq.h`, plain
  `wait_event`), woken by the producer. There is no poll, no `proc_sleep(1)`, no
  `proc_yield` spin. This is CLAUDE.md preference case 1: the wake source is
  entirely ours, so no wake can be lost and no timeout is needed.
- The producer side is non-blocking (ring write plus wake), so it is safe from a
  context with interrupts off or a spinlock held. `wq_assert_may_block()` would
  catch a violation, and the design has no path that appends from such a context.
- The enforcement query (Q1) touches only the in-memory fold and never waits at
  all, which is what lets it live at the syscall chokepoint.
- Slice 1 reuses the **existing** `seclog` worker rather than creating a second
  one. One more thread to do what an existing thread already does at the right
  priority, in the right context, at the right point in the boot, would be a
  private fork of a shared primitive.

---

## 14. Reproducing the measurements

```sh
cd kernel/fs/graphfs
grep -hoE "gfs_[a-z_0-9]+\(" node.h version.h query.h | sort -u | wc -l   # 72
cd ../../.. && grep -rn "gfs_" --include=*.c --include=*.rs . | grep -v fs/graphfs   # empty
grep -rn "blob_store_init\|blob_store_self_test" --include=*.c kernel/   # blob.c only

# sizeof under the kernel's own flags (binary search on _Static_assert)
gcc -ffreestanding -fno-pic -mno-red-zone -mcmodel=kernel -mno-mmx -mno-sse \
    -mno-sse2 -nostdlib -nostdinc -fno-builtin -I kernel -S -o /tmp/p.s -x c - <<'EOF'
#include "fs/graphfs/node.h"
float pf(const gfs_edge_t *e);
float pf(const gfs_edge_t *e) { return e->weight; }
EOF
# -> error: SSE register return with SSE disabled
```

---

## 15. What is built, and what is deliberately not

### Slice 1, implemented (see section 16 for the evidence)

The tamper-evident journal spine, end to end, with the **existing security audit
subsystem as its first genuine consumer**: `seclog`'s worker already receives every
`security_audit()` event, and there is already a Ring-3-reachable producer (a bad
user pointer to any syscall raises `AUDIT_PTR_INVALID`). So slice 1 has a real
caller on day one rather than being 72 functions looking for a user.

Implemented: record format, encode, chain verify, seal, boot generation counter,
in-kernel head anchor, DEGRADED fail-closed state, `SYS_GFS_VERIFY`, and a Ring-3
app that tampers with the journal four ways and shows each one caught.

### Deliberately NOT built, and why

- **Nodes, edges, versions, the fold, and 68 of the 72 declarations.** The journal
  is the spine everything else hangs from; building the graph before the spine is
  provably durable and tamper-evident would repeat the exact failure this ticket
  exists to correct. Slice 2 is the fold plus Q1-Q3.
- **The blob store dependency.** Seven measured defects (section 11) and it has
  never executed. Slice 1 stays self-contained so that "the journal works" is not
  contingent on "the blob store works". Repairing blob.c is its own slice with its
  own first execution.
- **A signed seal.** It needs a key in the immutable core (#305) and a non-root
  session (#679), both of which are already prerequisites in the architecture's own
  build order. Section 9.3 says exactly what is and is not defended without it.
- **Contract issuance, the escrow check, and the enforcement hook.** Those belong
  to the contract layer's own tickets. GraphFS's job is to make them storable,
  queryable and undoable, and to be honest about the fact that storing them is not
  the same as enforcing them.
- **Wall-clock time.** Not a graphfs feature; a prerequisite gap that graphfs
  surfaced. Section 6.1.
- **Anything with a float in it.** Section 2.

---

## 16. Disposition of the 72 existing declarations

The prior declarations are one author's guess at an API, not a specification. Every
one is dispositioned. Totals: **7 implement, 19 redesign, 46 delete.**

Legend: **I** implement broadly as declared (signature may tighten); **R** redesign
(the intent survives, the shape does not); **D** delete.

### node.h (29)

| # | declaration | | disposition and reason |
|---|---|---|---|
| 1 | `gfs_node_init` | R | one `gfs_init()`. Three separate init functions imply three stores; there is one journal |
| 2 | `gfs_node_create` | R | becomes journal op `NODE_CREATE`. No direct-create API, because a second write path is what §12.9 forbids |
| 3 | `gfs_node_get` | R | returns a small copied view, not a pointer into a shared 3,832-byte refcounted struct with a caller-visible spinlock |
| 4 | `gfs_node_get_by_path` | R | becomes `gfs_resolve_name(parent, name)` over NAME edges. GraphFS is not path-keyed; ext2 owns paths |
| 5 | `gfs_node_release` | D | no refcounted handles to release once lookups copy out |
| 6 | `gfs_node_update` | R | becomes journal op `VERSION_COMMIT` |
| 7 | `gfs_node_delete` | R | becomes `NODE_RETIRE`. Append-only, so "delete" can only ever be a state change |
| 8 | `gfs_node_purge` | D | destroying audit history is exactly what §12.6 forbids. Reclaiming unreferenced *content* is `blob_gc`, which is a different operation |
| 9 | `gfs_node_restore` | R | becomes `NODE_RESTORE` |
| 10 | `gfs_edge_create` | R | drop `float weight` (unsound, section 2); add `scope_id`/`expiry`; `rel` becomes an enum. This is where contracts live |
| 11 | `gfs_edge_remove` | R | becomes `EDGE_REVOKE`: sets `revoked_seq`, removes nothing |
| 12 | `gfs_edges_get_outgoing` | I | this is Q2, "what does H hold". Bounded out-array, `rel` as enum |
| 13 | `gfs_edges_get_incoming` | I | this is Q3, "what is granted on T" |
| 14 | `gfs_tag_add` | D | tags are a search feature with no contract consumer |
| 15 | `gfs_tag_remove` | D | as above |
| 16 | `gfs_tag_has` | D | as above |
| 17 | `gfs_node_set_description` | D | free text for LLM discovery; also a mutation that bypasses versioning |
| 18 | `gfs_node_set_mime_type` | D | file typing belongs to VFS/userland |
| 19 | `gfs_node_set_embedding` | D | `float[128]`; unsound in this kernel (section 2), and vector search in Ring 0 is a product nobody specified |
| 20 | `gfs_compute_hash` | D | duplicate of `blob_hash_compute`. A private fork of a shared primitive is forbidden |
| 21 | `gfs_hash_to_hex` | D | duplicate of `blob_hash_to_hex` |
| 22 | `gfs_path_parse` | D | `rustkern/vfs_path.rs` already owns path parsing |
| 23 | `gfs_path_validate` | D | as above |
| 24 | `gfs_path_parent` | D | as above |
| 25 | `gfs_path_filename` | D | as above |
| 26 | `gfs_node_lock` | D | a caller-visible spinlock invites holding it across I/O, the known blind spot of `wq_assert_may_block` |
| 27 | `gfs_node_unlock` | D | as above |
| 28 | `gfs_node_get_stats` | I | kept and extended: must count unlimited-scope and unlimited-expiry grants (§10) |
| 29 | `gfs_node_print` | R | folded into one `gfs_debug_dump()` |

### version.h (20)

| # | declaration | | disposition and reason |
|---|---|---|---|
| 30 | `gfs_version_init` | D | one `gfs_init()` |
| 31 | `gfs_history` | R | keyed by node id, not path; collapses into #32 |
| 32 | `gfs_history_by_node` | I | this is Q4/Q5's backbone |
| 33 | `gfs_checkout` | R | returns a content hash, not a node pointer. Reading bytes is the blob store's job |
| 34 | `gfs_checkout_by_node` | D | duplicate of #33 once path variants are gone |
| 35 | `gfs_revert` | R | keyed by node id; appends a new version. This is §7's rollback primitive |
| 36 | `gfs_revert_by_node` | D | duplicate of #35 |
| 37 | `gfs_diff` | D | text diffing does not belong in Ring 0. The kernel gives two content hashes; userland diffs |
| 38 | `gfs_diff_nodes` | D | as above |
| 39 | `gfs_diff_free` | D | as above |
| 40 | `gfs_version_info` | I | by node + version |
| 41 | `gfs_version_count` | I | |
| 42 | `gfs_version_at_time` | R | becomes `gfs_version_at_seq`. There is no wall clock (MEASURED), and journal position is the correct rollback key anyway |
| 43 | `gfs_version_compact` | R | becomes `gfs_checkpoint()`: an append-only chain is compacted by writing a signed checkpoint, never by deleting records |
| 44 | `gfs_branch_create` | D | branching serves no requirement in the architecture |
| 45 | `gfs_branch_merge` | D | as above, and merge conflict resolution in Ring 0 is a large attack surface for zero benefit |
| 46 | `gfs_branch_list` | D | as above |
| 47 | `gfs_hash_compare` | D | duplicate of `blob_hash_compare` |
| 48 | `gfs_version_print` | R | folded into `gfs_debug_dump()` |
| 49 | `gfs_diff_print` | D | with the diff engine |

### query.h (23)

| # | declaration | | disposition and reason |
|---|---|---|---|
| 50 | `gfs_query_init` | D | one `gfs_init()` |
| 51 | `gfs_query` | R | the 5,192-byte omni-query struct is replaced by the six named queries of section 7. A generic filter engine in Ring 0 answers nothing the contract layer asks and is unbounded work at the chokepoint |
| 52 | `gfs_query_free` | D | queries write into caller-supplied fixed arrays; Q1 must not allocate |
| 53 | `gfs_query_init_default` | D | with the query struct |
| 54 | `gfs_query_add_filter` | D | with the query struct |
| 55 | `gfs_get_related` | R | becomes bounded `gfs_edges_get_*` at depth 1; multi-hop is Q6 only |
| 56 | `gfs_get_related_by_path` | D | path-keyed variant |
| 57 | `gfs_search_tags` | D | search feature, no consumer |
| 58 | `gfs_tags_list` | D | as above |
| 59 | `gfs_search_tag` | D | as above |
| 60 | `gfs_search_description` | D | as above |
| 61 | `gfs_search` | D | as above |
| 62 | `gfs_search_content` | D | unbounded I/O in Ring 0, and a natural capability-bypass ("search everything, tell me what matched") |
| 63 | `gfs_nlp_resolve` | D | not implementable in-kernel (no model), and letting a fuzzy match choose which object a contract applies to is a security hole. Its `confidence` is a float too |
| 64 | `gfs_explain_relationship` | R | the *idea* is Q6 and is genuinely needed ("why does this app have access to that?"). Becomes a bounded walk over GRANT edges with no free-text summary |
| 65 | `gfs_suggest_related` | D | recommendation engine |
| 66 | `gfs_get_summary` | D | LLM context building belongs in Ring 3 |
| 67 | `gfs_search_similar` | D | float threshold plus embeddings |
| 68 | `gfs_find_similar` | D | as above |
| 69 | `gfs_query_get_stats` | I | merged into `gfs_stats` |
| 70 | `gfs_query_reset_stats` | D | resettable audit-adjacent counters are a footgun |
| 71 | `gfs_query_print` | D | with the query struct |
| 72 | `gfs_query_result_print` | D | with the query struct |

**The headers are not deleted in this change.** They are dead declarations with no
callers, so deleting them is a separate, individually-verified slimming step of the
kind #713 does properly; doing it in the same commit as new code would mix a
deletion with a feature and make the rollback of either harder.

---

## 17. Build order

1. **Slice 1 (this change): the journal spine.** Format, chain, seal, in-kernel
   anchor, fail-closed init, first real producer, Ring-3 verification.
2. **Slice 2 (DONE, section 19): the fold.** Nodes, edges, contracts in memory;
   replay at boot; Q1-Q3; `gfs_stats` with the §10 unlimited-contract counters.
   **CORRECTION to this plan, found while building it: VERSIONS ARE NOT IN SLICE
   2.** A `VERSION_COMMIT` payload is a 32-byte content hash and a node id, which
   does not fit the 16 inline bytes (5.3.1), so folding versions REQUIRES the
   blob store. Building half a version chain from a payload the fold cannot see
   would have been the exact failure this ticket exists to correct, so
   `VERSION_COMMIT` returns `E_UNSUPPORTED` today, loudly, and versions move to
   slice 3 behind the blob-store repair they actually depend on.
3. **Slice 3: blob.c repair and first execution** (B1-B7), then payload storage for
   records over 16 bytes.
4. **Slice 4: rollback.** Q4/Q5, `gfs_revert`, and the effect-class accounting that
   makes §7's "the undo surface must never lie" checkable.
5. **Slice 5: the signed seal**, once #305/#679 provide somewhere to keep a key.
6. Only then: contract issuance and the enforcement hook, which are the contract
   layer's tickets, not this one's.

---

## 18. Slice 1: what was built, and the evidence it ran

MEASURED on 2026-08-06, kernel build 1713, throwaway VM 2712 (kvm64, USB-MSC
live boot of golden 1025 with this kernel and one test app overlaid; VM and both
LVs destroyed afterwards).

### 18.1 What exists now

| file | role |
|---|---|
| `kernel/rustkern/gfsjournal.rs` | Rust: record format, encode, SHA-256 chain, seal, ALL verification. 5 exports, declared in `rust-symbols.manifest` |
| `kernel/fs/graphfs/journal.c` + `.h` | C: I/O and lifecycle glue only. Init (verify, fail closed), append, verify |
| `kernel/main.c` | `gfs_journal_init()` before `seclog_init()` |
| `kernel/security/seclog.c` | the first real producer: every security audit event also appends a journal record binding SHA-256 of that exact log line |
| `kernel/proc/syscall.{h,c}` | `SYS_GFS_VERIFY` (360), read-only, `copy_to_user` |
| `kernel/rustkern/argtab.rs` + `proc/syscall_argtab_lock.c` | the 80-byte out-struct is pointer-validated and size-locked |
| `userland/apps/gfstest/` | the Ring-3 prover |

Linked, not merely compiled: `nm obj/fs/graphfs/journal.o` shows
`T gfs_journal_{init,append,verify,ready}`; `nm librustkern.a` shows
`T gfsj_{encode,seal_encode,seal_bootgen,verify,sizes}`; `strings kernel.elf`
finds 13 `[GFSJ]` messages in the linked image.

### 18.2 Ring-3 output, boot 1 (verbatim from `/GFSTEST.OUT` on the ext2 root)

```
== gfstest: GraphFS journal tamper detection, from Ring 3 (#711) ==
PASS  baseline  OK: 1 records, boot generation 1, degraded=0
INFO  snapshot: log 160 bytes (1 records), seal 96 bytes
PASS  producer  a Ring-3 syscall grew the journal: 1 -> 3 records
PASS  truncate  RED: TRUNCATED(9) at seq 2  [disk 2, sealed 3]
PASS  reorder   RED: CHAIN_BREAK(4) at seq 0  [disk 3, sealed 0]
PASS  edit      RED: HASH_MISMATCH(6) at seq 0  [disk 3, sealed 0]
PASS  rollback  RED: ANCHOR_COUNT(12) at seq 3  [disk 1, sealed 1]
PASS  restore   journal verifies again: 3 records
== gfstest: 7 passed, 0 failed ==
```

The kernel logged each detection independently on serial, so the evidence exists
in two places the test app cannot both forge:

```
[GFSJ] VERIFY FAILED: TRUNCATED (records removed) at seq 2 (on disk 2, sealed 3, kernel 3)
[GFSJ] VERIFY FAILED: CHAIN_BREAK (reordered or spliced) at seq 0 (on disk 3, sealed 0, kernel 3)
[GFSJ] VERIFY FAILED: HASH_MISMATCH (a record was edited) at seq 0 (on disk 3, sealed 0, kernel 3)
[GFSJ] VERIFY FAILED: ANCHOR_COUNT (differs from kernel memory) at seq 3 (on disk 1, sealed 1, kernel 3)
```

Each check is proven RED against the real artifact, and green again afterwards
(the `restore` line), which is what distinguishes a check from a constant.

**`producer` is the line that matters most.** The journal grew because a Ring-3
process made a syscall carrying a kernel pointer, the #500 validator rejected it,
`security_audit()` raised the event, and the seclog worker appended it. There is
a real consumer on day one, and that consumer is reachable from userland.

**`rollback` is the strongest result.** The app restored an EARLIER, genuine
`(log, seal)` pair the kernel itself had written. Both files are internally and
mutually consistent, so neither the hash chain nor the seal can object, and no
crypto was needed in Ring 3 to build the attack. Only the in-kernel head anchor
caught it (9.3).

### 18.3 Persistence and continuity across a reboot (boot 2)

```
[GFSJ] journal verified: 3 record(s), boot generation 2
[GFSJ] up: /GRAPHFS/JOURNAL.LOG (4 record(s)), seal /GRAPHFS/JOURNAL.SEAL
...
PASS  baseline  OK: 4 records, boot generation 2, degraded=0
== gfstest: 7 passed, 0 failed ==
```

The previous boot's records survived, verified, and the boot generation advanced.

### 18.4 The fail-closed path, proven with an OFFLINE tamper (boot 3)

With the VM stopped, one byte inside record 2 was flipped directly on the ext2
partition from the host, i.e. by an attacker with the disk and no running kernel.

```
[GFSJ] TAMPER/CORRUPTION DETECTED at startup: HASH_MISMATCH (a record was edited)
       at seq 2 (on disk 6 record(s), seal claims 0). Preserving the evidence as
       /GRAPHFS/JOURNAL.BAD and starting a new chain.
[GFSJ] up: /GRAPHFS/JOURNAL.LOG (2 record(s)), seal /GRAPHFS/JOURNAL.SEAL
```

and Ring 3 saw the sticky degraded state, `PASS baseline OK: 2 records, boot
generation 3, degraded=1`. On disk afterwards: `JOURNAL.BAD` (960 bytes, the six
original records) and `SEAL.BAD` preserved; the new chain's record 0 is
`op=3 (CHECKPOINT)` with inline payload `06 00 00 00 02 00 00 00` = reason 6
(HASH_MISMATCH) at seq 2. The discontinuity is itself recorded, in the journal,
in a form that verifies.

### 18.5 The format is hand-checkable

Record 0 of a fresh chain, straight off the ext2 partition:

```
0000000 47 46 4a 31 01 00 a0 00 00 00 00 00 00 00 00 00   GFJ1, v1, len 160, seq 0
0000016 01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00   boot_gen 1, mono_ms 0
0000032 00 00 00 00 00 00 00 00 01 00 03 00 08 00 00 00   actor kernel/0, op 1 (BOOT), effect 3, plen 8
0000048 7c 9f a1 36 ...                                   payload_hash
0000080 00 00 00 00 ...                                   prev_hash: zero, this is seq 0
0000112 01 00 00 00 ...                                   inline payload: boot generation 1
0000128 99 2d 64 07 ...                                   self_hash
```

and record 1's `prev_hash` at offset 80 is byte-identical to record 0's
`self_hash`, which is the chain, visible without any tooling.

### 18.6 What this slice does NOT prove

Stated so a green run is not over-read:

- It does not prove the ADAPTIVE attack is caught **across a reboot**. It cannot
  be, without a signed seal (9.3). Boot 3 caught an offline non-adaptive edit; an
  offline attacker who recomputes the chain and the seal would not be caught, and
  that is why 9.3 names the signature and its two prerequisites.
- It does not prove anything about contracts, nodes, edges or rollback. None of
  those exist yet (15).
- It does not prove `blob.c` works. Slice 1 deliberately never calls it (11).
- The journal is capped at 16,384 records with no rotation. Beyond that, appends
  are refused, loudly and counted, and that is a real gap until slice 4.
- The human-readable audit text still lives in `/CONFIG/SECURITY.LOG`, which is
  rewritten whole on every drain and is NOT append-only. What is tamper-evident
  today is the journal's record of it: each record binds SHA-256 of the exact log
  line. Detecting an edit to `SECURITY.LOG` by comparing it against the journal
  is possible with what exists but is not implemented.

---

## 19. Slice 2: the fold, and the evidence it runs

MEASURED on 2026-08-07, kernel builds 1754 (green), 1755 (deliberately broken)
and 1756 (restored, green again), throwaway VM 2716 (kvm64, USB-MSC live boot of
golden 1744 with this kernel and one test app overlaid; VM and both LVs destroyed
afterwards).

### 19.1 What exists now

| file | role |
|---|---|
| `kernel/rustkern/gfsfold.rs` | Rust: the graph payload layouts, the node/edge tables, `gfsf_apply` (the ONE function that changes state), the escrow rule, Q1/Q2/Q3, stats. 15 exports, declared in `rust-symbols.manifest` |
| `kernel/fs/graphfs/fold.{c,h}` | C: the lock, the clock, the call into the journal, the boot self-test, the read-only Ring-3 handler |
| `kernel/fs/graphfs/journal.c` | one new call site: every appended record is folded, and `gfs_journal_append_seq()` reports the seq that becomes an edge's identity |
| `kernel/main.c` | `gfs_fold_init()` immediately after `gfs_journal_init()` |
| `kernel/proc/users.c` | the first identity producer outside GraphFS: `user_create()` appends a `NODE_CREATE` |
| `kernel/proc/syscall.{h,c}`, `rustkern/argtab.rs`, `proc/syscall_argtab_lock.c` | `SYS_GFS_QUERY` (363), READ-ONLY, arg-driven write extent, three struct sizes locked |
| `userland/apps/gfstest/` | extended with nine Ring-3 slice-2 checks, including two boundary attacks |

### 19.2 The property this slice exists to establish

**State is a fold of the journal, by construction.** There is exactly one
function that mutates graph state, `gfsf_apply`, and its only argument is a
160-byte journal record. It is reached from exactly two places: replay at boot,
over bytes read off the disk, and `gfs_journal_append()`, immediately after the
record it just wrote became durable. There is no "write the node table" entry
point, because there is no node table API.

That makes §12.9's "no bypass path" a structural fact rather than a discipline.
It also means the pre-check and the real apply cannot disagree: a mutation is
validated by calling the SAME function with `dry_run=1`, so a refusal costs no
journal space and runs the identical policy code.

### 19.3 Boot 1, generation 1: kernel and Ring 3, both on serial

```
[GFSJ] journal verified: 0 record(s), boot generation 1
[GFSF] fold replayed 0 record(s) from the verified journal (0 applied, 0 refused), boot generation 1
[GFSJ] up: /GRAPHFS/JOURNAL.LOG (1 record(s)), seal /GRAPHFS/JOURNAL.SEAL
[GFSF] identities: 3 user account(s) present in the graph
[GFSF] selftest: 16/16 - deny before grant, 8 refusal rules, allow in scope,
       deny out of scope, deny reversed, deny expired, deny after revoke
       [checks=6 allow=1 deny=5 refused: not-escrow=2 bad-scope=1 bad-expiry=1]
[GFSF] up: 8 node(s), 1 edge(s), 0 live grant(s) (0 unlimited-scope, 0 never-expire),
       11 record(s) folded, 0 refused, 0 prior-boot grant(s) expired
```

and from Ring 3, through the read-only syscall, in the same serial stream:

```
PASS  fold-live ready=1 degraded=0 overflow=0, 11 record(s) folded, 8 node(s), 1 edge(s), boot gen 1
PASS  identities 8 node(s): kernel=1 escrow=1, 3 USER identit(ies) from the real account database
PASS  enforced  6 check(s): 1 allowed, 5 denied; refusals fired: not-escrow=2 bad-scope=1 bad-expiry=1
PASS  q1-revoked the self-test's revoked contract does not grant: check(app -> object, scope 255) = DENY
PASS  q1-unknown check(unknown -> unknown) = DENY
PASS  q3-visible 1 edge(s) into the object, the revoked grant still listed with its revoking seq
PASS  no-append 19 undefined command ids changed nothing: 8 node(s), 1 edge(s), 11 record(s) folded
PASS  forge-grant a forged escrow GRANT written straight into the journal file grants NOTHING (check=0/0)
PASS  forge-seen and the forgery is detected: CHAIN_BREAK(4) at seq 14
== gfstest: 16 passed, 0 failed ==
```

Two lines carry most of the weight.

**`forge-grant` is the boundary test.** `/GRAPHFS/JOURNAL.LOG` is an ordinary
ext2 file and Ring 3 can write it, deliberately (section 10: an audit trail
nobody can inspect is not an audit trail). So the test WRITES a well-formed
`EDGE_ADD` record into it, claiming to be from the escrow, granting this app's
identity `SCOPE_ANY` with `EXPIRY_NEVER` over the target. If the graph were read
from the file, that would be a total bypass of the contract model. It is not: the
fold is memory the kernel builds from records IT appended, so the forgery grants
nothing, and the chain reports it as tampering. Both halves are checked.

**`identities` is the anti-"zero callers" evidence.** The three USER nodes are
the machine's actual accounts, read from the real user database, not fixtures.
The producer is `user_create()`, so creating an account from Ring 3 puts a node
in an append-only trail that Ring 3 cannot write.

### 19.4 Boot 2, generation 2: replay, persistence, and fail-closed expiry

```
[GFSJ] journal verified: 14 record(s), boot generation 2
[GFSF] fold replayed 14 record(s) from the verified journal (14 applied, 0 refused), boot generation 2
[GFSF] selftest: 16/16 ... [checks=6 allow=1 deny=5 refused: not-escrow=2 bad-scope=1 bad-expiry=1]
[GFSF] up: 8 node(s), 1 edge(s), ... 13 record(s) folded, 0 refused, 1 prior-boot grant(s) expired
```
```
PASS  prior-boot boot generation 2: 1 grant(s) from earlier boots expired by the
      fold (no trusted clock)
== gfstest: 17 passed, 0 failed ==
```

The whole graph was rebuilt from the record stream with zero refusals, the eight
identities persisted, and **the previous boot's grant was expired** rather than
honoured. That is section 6.1 made operational: with no trusted clock an expiry
from another boot cannot be evaluated, so it fails closed. Identities persist;
authority does not.

### 19.5 The self-test is a real gate: proven RED, by name

A check only ever seen to pass is indistinguishable from a constant, so the
escrow rule was DELETED (`if false && (...)`, build 1755) and the kernel rebuilt
and booted:

```
[GFSF] SELFTEST FAIL refuse-not-escrow: got 0 (OK), expected -7 (NOT_ESCROW (only
       the escrow may issue a GRANT))
[GFSF] SELFTEST FAILED: 15/16 checks. The enforcement path is NOT trustworthy on
       this build, so the fold is marked DEGRADED and every grant check will DENY.
```

and Ring 3 saw the consequence independently, without being told:

```
FAIL  fold-live ready=1 degraded=1 overflow=0, ...
PASS  enforced  ... refused: not-escrow=1 bad-scope=1 bad-expiry=1
== gfstest: 15 passed, 1 failed ==
```

Three things are proven at once: the self-test names the exact rule that was
removed; a failure marks the store DEGRADED, which makes every grant check deny;
and the refusal counter drops from 2 to 1, which is the precise expected delta
because only the `EDGE_ADD` check was disabled and the `EDGE_REVOKE` one still
fires. The rule was restored and build 1756 booted green again (16/16, 16 passed
0 failed), so the RED is attributable to the deletion and nothing else.

### 19.6 What this slice does NOT prove

Stated so a green run is not over-read.

- **No contract is enforced anywhere yet.** GraphFS can now store, answer and
  revoke contracts. NOTHING in the kernel calls `gfs_grant_check()` to decide
  whether an operation is allowed. Wiring the enforcement hook at the syscall
  chokepoint is the contract layer's ticket, not this one, and until it exists
  the correct description is "the substrate is ready", never "contracts are
  enforced".
- **There is no escrow.** `GFS_NODE_ESCROW` is a node id the fold checks against;
  it is not a protected component. Until the immutable security core (#305) owns
  it, any kernel code can pass that id. The rule being enforced in one place is
  what makes it possible to move later; it is not the same as it being safe now.
- **The only GRANT issuer today is the boot self-test.** That is deliberate and
  it is stated in the audit trail: the two records it writes per boot carry a
  reserved scope and are as visible as any other. A self-test that hid its own
  writes would be arguing against the trail's purpose.
- **Versions, `gfs_revert`, Q4, Q5 and Q6 do not exist.** `VERSION_COMMIT`
  returns `E_UNSUPPORTED`. See the correction in section 17.
- **The tables are fixed at 512 nodes and 1024 edges** and overflow is sticky and
  makes Q1 deny everything. That is fail-closed and it is also a real ceiling: an
  identity-per-process producer would reach it. Rotation and compaction are slice
  4's, along with the journal's own 16,384-record cap.
- **The fold inherits slice 1's honest boundary exactly.** Within a boot, Ring 3
  cannot forge a contract (19.3). Across a reboot the in-kernel anchor is gone,
  so an OFFLINE attacker who rewrites the chain AND the seal consistently would
  have their forged `EDGE_ADD` replayed and folded. That is the unsigned-seal gap
  named in 9.3, and it is the single most important reason the signed seal
  (slice 5, needing #305 and #679) is a prerequisite for shipping contracts, not
  a nicety.
- **A hard `qm stop` can lose the journal's tail.** MEASURED during this work: a
  VM killed shortly after a write came back with a log and seal that were both
  two records short and MUTUALLY CONSISTENT, so verification passed. Append-only
  is not durability; §12.6 asks for tamper evidence and gets it, but a power cut
  can still cost the most recent records.
