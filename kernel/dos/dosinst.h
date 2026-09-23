// dosinst.h - per-DOS-instance registry (dosconcurrency, owner requirement
// 2026-09-16). Maps a hosting process id to its DOS guest state. The state is
// opaque here (void*) because dos_task_t is file-local to dosexec.c.
//
// Today exactly one in-kernel DOS guest runs at a time (g_dos_busy enforces
// it), and each Ring-3 /APPS/DOSUSER host is already address-space isolated, so
// this registry currently holds at most ONE live entry and dos_cur() resolves
// to the single g_dos. It is the identity-preserving container the
// concurrent-DOS work grows along: the existing single guest is instance 0.
#ifndef DOS_INST_H
#define DOS_INST_H

#include "../types.h"

// Bind pid -> guest state. Replaces an existing binding for the same pid.
// No-op for pid==0 or task==NULL.
void  dos_inst_register(uint32_t pid, void *task);
// Drop the binding for pid, if any.
void  dos_inst_unregister(uint32_t pid);
// Guest state bound to pid, or NULL if none.
void *dos_inst_lookup(uint32_t pid);
// Number of live bindings.
int   dos_inst_count(void);

// (dosconc4) FOCUS-OWNERSHIP of the shared HOST SINGLETONS (the raw keyboard tap
// in cpu/isr.c and the single OPL2/FM audio sink in dos/dosfmq.c). Exactly one
// DOS guest may drive these at a time; ownership follows compositor focus. These
// are read LOCKLESS on the input/audio path and set/cleared only at the rare
// focus edge, so NO spinlock lands on the input/audio hot path (owner brief /
// #426). 0 = no DOS owner.
void     dos_inst_set_focus_owner(uint32_t pid);   // claim (no-op for pid==0)
void     dos_inst_clear_focus_owner(uint32_t pid); // release, only if pid owns
uint32_t dos_inst_focus_owner(void);
// True when pid MAY drive the shared host singletons: it owns them, or nobody
// does. The "nobody does" arm keeps a LONE guest byte-identical.
int      dos_inst_may_drive_host(uint32_t pid);

#endif
