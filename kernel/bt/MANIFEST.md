# Task #372 - Bluetooth Stack ARCHITECTURE + Skeleton - MANIFEST

Date: 2026-07-03
Role: ARCHITECT (contract + compiling skeleton for 4 downstream agents)
Build server: the build container on the build server (<BUILD_SERVER>), gcc-12, -Werror
Kernel tree: <BUILD_PATH>/active-code/source/kernel/
Version after this work: 1.74.0 build 597 (MAYTERA_BUILD_NUMBER bumped 596 -> 597;
  note the repo's increment_build.sh targets a stale path and does NOT bump the
  active version.h, so the bump was manual).

## What was delivered

A layered Bluetooth stack SKELETON that compiles GREEN under -Werror and boots
with Bluetooth gated OFF (g_bt_enable default 0). This is the API CONTRACT the
transport and protocol agents build against.

### New directory: bt/ (in the kernel tree)

Headers (ARCHITECT-owned contract, STABLE):
  bt.h           top-level init/state, bt_addr_t, return codes, g_bt_enable
  bt_transport.h transport ops the USB driver implements + RX upcalls
  hci_defs.h     HCI opcodes / event codes / packet headers / error codes
  hci.h          HCI layer: cmd/event/ACL, connection table, observers
  l2cap.h        L2CAP channels, PSMs, fixed CIDs, server/connect/send
  sdp.h          SDP client (HID query) + server
  hid.h          HID profile (HIDP classic + HOGP BLE) + input choke point
  pair.h         pairing (SSP classic + SMP BLE)
  bt_ctrl.h      control API for UI/syscalls (power/scan/pair/connect/forget)

Stub .c (compile green, return BT_ERR_NOTIMPL):
  bt.c       ARCHITECT: top-level, transport registry, control API (real logic)
  hci.c      PROTOCOL agent
  l2cap.c    PROTOCOL agent
  sdp.c      PROTOCOL agent
  hid.c      PROTOCOL agent (mouse input funnel wired; keyboard is a TODO)
  pair.c     PROTOCOL agent
  hci_usb.c  TRANSPORT agent (defines bt_usb_probe, gated by g_bt_enable)

Docs:
  ARCHITECTURE.md   the layered design, Classic vs BLE, phasing, ownership
  INTEGRATION.md    file ownership, the build container build-serialization, HID->compositor

### Build wiring (Makefile on the build container)

Added: BT_DIR=bt, BT_SOURCES=$(wildcard $(BT_DIR)/*.c), $(BT_SOURCES) in
ALL_C_SOURCES, and mkdir of obj/bt in the dirs target. Picked up automatically
by the generic pattern rule; new bt/*.c need no further Makefile edit.

### Boot wiring (main.c)

Gated call after sched_set_preemption(true), next to heartbeat/net worker:
    if (g_bt_enable) { bt_init(); }
bt_init() itself also returns BT_ERR_DISABLED when the flag is 0 (double gate).

### Reconciliation with the concurrent transport agent

The transport agent had already added a call to bt_usb_probe(xhci_controller_t*,
slot, speed, vid, pid, class, subclass, proto, cfg, total) in drivers/xhci.c
(mirroring usb_net_probe). The architect FROZE that as the canonical enumeration
entry and defined the matching stub in bt/hci_usb.c, gated by g_bt_enable so
boot-time USB enumeration is a hard no-op when Bluetooth is off.

## Verification

- Isolated -Werror compile of all 7 bt/*.c: PASS.
- Full kernel build + link on the build container: GREEN (kernel.elf ~12MB, build 597).
  Only ld warning is the pre-existing benign .note.GNU-stack note.
- Boot smoke test on VM 2061 (OVMF/pc, e1000, 8 cores): desktop rendered
  (icons, taskbar, wallpaper, compositor), heartbeat advancing (uptime 30->34s,
  flips 122->232, ctxsw climbing = alive, no freeze), ZERO [BT] serial output
  (bt correctly inert), no panic/fault. Screendump: vm2061-desk.png.

## Files in this golden dir

  bt/*.h, bt/*.c        the full skeleton (contract + stubs)
  ARCHITECTURE.md, INTEGRATION.md, MANIFEST.md
  vm2061-boot.log       serial boot log (bt disabled)
  vm2061-desk.png       desktop screendump proving healthy boot

## Kernel artifact

  the build container: <BUILD_PATH>/active-code/source/kernel/kernel.elf (build 597)
  Host copy used for boot test: a /tmp copy on the build server
