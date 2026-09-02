#ifndef _KERNEL_GUI_PRESENTSCALE_H
#define _KERNEL_GUI_PRESENTSCALE_H
// presentscale.h - the C face of integer PRESENT-SCALE compositing (#halfres).
//
// One owner's 3840x2160 panel offers seven firmware modes, of which the only
// 16:9 one is native 4K (the rest are 4:3/5:4). He is already running the UI
// at 200% scale - every widget already drawn at double size into 8.29 Mpx -
// so compositing at 1920x1080 (scale 100%) and presenting with an EXACT
// integer 2x pixel replication gives the identical apparent picture for a
// quarter of the compositing work, with no resampling softness.
//
// This file is the plumbing: where the factor comes from (config / an ESP
// override file, mirroring uiscale.c's own DISPLAY.CFG + /UISCALE.TXT
// pattern exactly, including its hard-won lesson about which path spellings
// actually reach the FAT partition), and what has to happen at boot to apply
// it. The pure "is this factor valid for this panel" arithmetic and its
// property self-test live in rustkern/presentscale.rs, mirroring the
// uiscale.c (C plumbing) / uiscale.rs (Rust arithmetic) split. The actual
// per-pixel replication lives in kernel/video/framebuffer.c, next to the
// display-rotation present path it structurally mirrors.
//
// OFF BY DEFAULT. Absent both the ESP override and the config key, this is a
// complete no-op: fb_get_present_scale() stays 1 and every present takes its
// pre-existing, unchanged code path. This is one owner's preference for one
// physical panel, not a general policy.

#include "../types.h"

// Boot-time bring-up. Reads /CONFIG/DISPLAY.CFG's `present_scale=<n>` key and
// the ESP override files, validates the requested factor against the REAL
// panel (rustkern/presentscale.rs), applies it via fb_set_present_scale(),
// and logs the effective value durably either way (ON, OFF, or REFUSED and
// why). Called once from the GUI bring-up path, AFTER the framebuffer and the
// root filesystem are up, and BEFORE uiscale_init() - uiscale must see the
// REDUCED logical dimensions this call may have just produced, not the
// physical panel, or a 1920x1080 logical surface on a 3840x2160 panel would
// be auto-scaled to 200% on top of the already-2x present, i.e. four times
// too large. See kernel/gui/desktop.c's call order.
void presentscale_init(void);

// The active integer factor. 1 means off (the default, and what any machine
// that never asked for this stays at).
int32_t presentscale_active_n(void);

// A one-line human description of where the live value came from (or why a
// request was refused), for the boot log and for Settings. Never NULL.
const char *presentscale_src_name(void);

#endif // _KERNEL_GUI_PRESENTSCALE_H
