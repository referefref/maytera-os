// dev.h - In-kernel "device" namespace (Phase A2)
//
// A minimal flat name -> file_ops map. Used to look up virtual file backends
// that don't live on the FAT partition:
//   - "console" -> drivers/console.c (kernel console + syslog)
//   - "tty"     -> Phase C1 (controlling tty of the current session)
//   - "pts/N"   -> Phase F (pty slave N)
//   - "ptmx"    -> Phase F (pty master allocator)
//
// Look-up is keyed by the path tail after "/dev/". Names are matched exactly
// and case-sensitively (FAT paths are uppercased upstream, but /dev/ lookups
// are NOT routed through FAT so we see the original case).

#ifndef DRIVERS_DEV_H
#define DRIVERS_DEV_H

#include "../types.h"

struct file;
struct file_ops;

// Factory signature: called when user opens /dev/<name> with the given
// flags. Returns a refcounted struct file* on success, NULL on failure.
// Many devices just call file_alloc(&fops, priv, flags); ptmx is the
// exception (it manufactures a fresh slave on every open).
typedef struct file *(*dev_open_fn)(int flags);

// Register a device under /dev/<name>. Must be called from kernel init,
// before any process can open() it. Returns 0 on success, -1 on dup name
// or table full.
int dev_register(const char *name, dev_open_fn open);

// Resolve /dev/<name> and call the registered factory. Returns NULL for
// unknown names or factory failure.
struct file *dev_open(const char *name, int flags);

// Initialize the /dev/ namespace. Registers "console".
void dev_init(void);

#endif // DRIVERS_DEV_H
