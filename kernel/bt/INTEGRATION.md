# MayteraOS Bluetooth Integration Plan (#372)

How the four agents share the `bt/` tree without stepping on each other, and
how the pieces bolt into the existing kernel.

## 1. File ownership

Do NOT edit a file you do not own. The headers are the contract: once
published they are STABLE. If you need a contract change, ask the architect;
do not silently edit a header other agents compile against.

| File | Owner | Notes |
|------|-------|-------|
| `bt/bt.h` | Architect | Top-level types, return codes, `bt_addr_t`, `g_bt_enable` |
| `bt/bt_transport.h` | Architect | Transport ops + RX contract |
| `bt/hci.h`, `bt/hci_defs.h` | Architect (defs) / Protocol (impl consumer) | Opcodes/events are frozen constants |
| `bt/l2cap.h` | Architect | Channel/PSM/CID contract |
| `bt/sdp.h`, `bt/hid.h`, `bt/pair.h` | Architect | Profile contracts |
| `bt/bt_ctrl.h` | Architect | Control API for UI/syscalls |
| `bt/bt.c` | Architect | Top-level, transport registry, control API |
| `bt/hci_usb.c` | Transport agent | USB enumeration + EP0/int-IN/bulk glue |
| `bt/hci.c` | Protocol agent | HCI command/event/ACL engine |
| `bt/l2cap.c` | Protocol agent | L2CAP signalling + channels |
| `bt/sdp.c` | Protocol agent | SDP client/server |
| `bt/hid.c` | Protocol agent | HIDP + HOGP, input funnel |
| `bt/pair.c` | Protocol agent | SSP + SMP |

The transport agent also owns the enumeration entry point `bt_usb_probe()`
(signature frozen to match the call site in `drivers/xhci.c`, mirroring
`usb_net_probe`) and its wiring in `drivers/xhci.c`'s class-driver dispatch
(outside `bt/`); coordinate that shared-file edit with the architect. The probe
is gated by `g_bt_enable` so boot-time enumeration is a hard no-op when
Bluetooth is off.

## 2. Build serialization rule (the build container)

All main-kernel builds happen in the build container on the build server,
tree `<BUILD_PATH>/active-code/source/kernel/`. The object
dir is shared, so only ONE `make` may run at a time.

Before building:
```
pct exec <ct> -- bash -lc "ps -eo comm | grep -E 'cc1|make|ld' | grep -v grep"
```
If that prints anything, another agent is building: wait and re-poll. Only when
it is empty do you run:
```
pct exec <ct> -- bash -lc "cd <BUILD_PATH>/active-code/source/kernel && make -j4"
```
Do not `make clean` casually; it forces every agent to rebuild ~7 MB of kernel.
`bt/*.c` is picked up automatically by the Makefile `$(wildcard $(BT_DIR)/*.c)`
rule (added this pass), so adding a new `bt/*.c` needs no Makefile edit.

## 3. Getting files onto the build container

Edit locally, then copy each changed file into the container. From the build server:
```
pct push <ct> /path/on/host/bt/hci.c \
  <BUILD_PATH>/active-code/source/kernel/bt/hci.c
```
Never bulk-sync; push individual files to the exact `bt/` path.

## 4. How HID reports reach the compositor input queue

This is the whole point of the stack. The path mirrors USB HID exactly:

```
BT dongle --(bulk/int IN)--> hci_usb.c --> bt_transport_deliver_acl/event
   --> hci.c (reassemble) --> l2cap.c (route to HID channel)
   --> hid.c on_data: strip HIDP/ATT header, get boot report
   --> bt_hid_input_report(kind, report, len)   <-- SINGLE choke point
         kind==MOUSE:    mouse_inject_hid(dx,dy,buttons,wheel)  [drivers/mouse.c]
         kind==KEYBOARD: HID usage -> set-1 scancode
                         -> keyboard_process_scancode(sc)       [cpu/isr.c]
   --> existing input queue --> compositor (identical to USB/PS2)
```

Rules:
- `bt_hid_input_report()` in `hid.c` is the ONLY function allowed to call
  `mouse_inject_hid` / `keyboard_process_scancode`. Keep it that way so there is
  one audited seam between Bluetooth and the input system.
- Reuse usb_hid.c's `hid_to_set1[]` usage->scancode table for the keyboard
  path. Factor it into a shared helper (e.g. `drivers/hid_keymap.h`) rather than
  duplicating the ~100-entry table; coordinate that refactor with the architect
  because it touches `drivers/`.
- Never call the input sinks from an interrupt handler or the compositor
  thread. Reports are processed inside `bt_poll()` on the bt worker thread.

## 5. Where the stack is pumped and started

`main.c`, after `sched_set_preemption(true)`, next to the heartbeat / net
worker start:
```c
// #372 Bluetooth: gated behind g_bt_enable (default 0). No-op unless enabled,
// so an incomplete stack can never affect boot.
if (g_bt_enable) {
    bt_init();
    // Phase 1+: proc_create("bt", bt_worker, NULL, PRIO_NORMAL);  // pumps bt_poll()
}
```
The `bt_worker` thread (a simple `for(;;){ bt_poll(); proc_sleep(N); }`) is
added by the transport agent in Phase 1 once there is real work to pump. Until
then `bt_init()` returns `BT_ERR_DISABLED` and nothing runs.

## 6. Contract stability

The headers in this directory are frozen for the other agents. Changes go
through the architect and are announced with a CHANGELOG entry. If a struct
must grow, append fields at the end; do not reorder or repurpose existing ones.
