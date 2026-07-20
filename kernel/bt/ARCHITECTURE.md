# MayteraOS Bluetooth Stack Architecture (#372)

Status: skeleton / API contract. Layers are stubs that compile green with
`-Werror`. Boot is unaffected: the whole stack is gated behind `g_bt_enable`
(default 0).

## 1. Goal

Bring a USB Bluetooth dongle up to the point where a Bluetooth keyboard and
mouse deliver input into the compositor, identical to how USB HID and PS/2
input already flow. Everything else (audio, file transfer) is out of scope for
the near term.

## 2. Layered model

```
   +--------------------------------------------------------------+
   |  UI / Settings app / SYS_BT_* syscalls                       |
   +--------------------------------------------------------------+
   |  Control API            bt_ctrl.h        (impl in bt.c)      |  ARCHITECT
   |    bt_power / bt_scan / bt_pair / bt_connect / bt_forget      |
   +--------------------------------------------------------------+
   |  Profiles                                                    |
   |    SDP        sdp.h / sdp.c     service discovery            |  PROTOCOL
   |    HID        hid.h / hid.c     HIDP (classic) + HOGP (BLE)  |  PROTOCOL
   |    Pairing    pair.h / pair.c   SSP (classic) + SMP (BLE)    |  PROTOCOL
   +--------------------------------------------------------------+
   |  L2CAP        l2cap.h / l2cap.c channels, PSM, fixed CIDs    |  PROTOCOL
   +--------------------------------------------------------------+
   |  HCI          hci.h / hci_defs.h / hci.c                     |  PROTOCOL
   |    commands, events, ACL reassembly, connection table       |
   +--------------------------------------------------------------+
   |  Transport    bt_transport.h    (registry glue in bt.c)     |  ARCHITECT
   +--------------------------------------------------------------+
   |  USB HCI      hci_usb.c         EP0 cmd, int-IN evt, bulk ACL|  TRANSPORT
   +--------------------------------------------------------------+
   |  xHCI / USB core (drivers/xhci.c, drivers/usb*.c)  EXISTING  |
   +--------------------------------------------------------------+
```

Top-level orchestration (`bt.c`) owns `bt_init()`, `bt_poll()`, the transport
registry, and the control API. `bt_poll()` is the single pump, run from a
dedicated low-priority kernel worker thread (like the net worker / heartbeat),
so no Bluetooth work ever runs on the compositor or an interrupt path.

## 3. Data flow

- Commands: `hci_send_cmd()` -> `transport->send_cmd()` -> EP0 control transfer.
- Events: interrupt-IN completion -> `bt_transport_deliver_event()` ->
  `hci_on_event()` -> command matching / observers (pairing) / connection table.
- ACL out: `l2cap_send()` -> `hci_send_acl()` (fragment to controller MTU) ->
  `transport->send_acl()` -> bulk-OUT.
- ACL in: bulk-IN completion -> `bt_transport_deliver_acl()` -> `hci_on_acl()`
  (reassemble fragments) -> `hci_acl_sink` -> `l2cap_input()` -> per-channel
  `on_data` -> profile (HID/SDP).
- HID input: profile parses the report and calls `bt_hid_input_report()`, the
  single choke point that funnels into the existing input path (see section 6).

## 4. Classic BR/EDR vs BLE, and the recommended path

Two independent link types share one HCI transport:

- Classic BR/EDR: inquiry -> page/connect -> SSP pairing -> SDP -> L2CAP HID
  control (PSM 0x0011) + interrupt (PSM 0x0013) -> HIDP boot reports. This is
  how older Bluetooth keyboards/mice work.
- BLE: LE scan -> LE connect -> SMP pairing -> GATT discovery -> HOGP (HID over
  GATT), input reports arrive as ATT notifications on the Report
  characteristic. This is how most modern Bluetooth input devices work.

Recommendation for keyboard/mouse:

1. Phase A (first working input): implement Classic BR/EDR + HIDP. It is the
   simpler, self-contained path (no GATT/ATT engine required) and is enough to
   validate the whole transport -> HCI -> L2CAP -> HID -> compositor chain end
   to end. Many test keyboards still support classic HID.
2. Phase B (broad device coverage): add BLE + SMP + a minimal ATT/GATT client +
   HOGP, because most current retail keyboards/mice are BLE-only. The `hid.h`
   API already has both attach paths (`bt_hid_attach_classic` /
   `bt_hid_attach_ble`) so the input choke point is shared and Phase B does not
   disturb Phase A.

`bt_hid_input_report()` is transport-agnostic on purpose: both HIDP and HOGP
produce the same boot-protocol report bytes, so once Phase A proves the input
funnel, Phase B only has to add the ATT plumbing that feeds the same function.

## 5. Phasing

| Phase | Deliverable | Owner |
|-------|-------------|-------|
| 0 | Skeleton + headers + build wiring + gated boot (THIS PASS) | Architect |
| 1 | USB transport: enumerate dongle, EP0 cmd, int-IN events, bulk ACL | Transport |
| 2 | HCI reset sequence, event decode, connection table, ACL reassembly | Protocol |
| 3 | L2CAP signalling + channels (classic connect/config) | Protocol |
| 4 | Classic SSP pairing + SDP HID query + HIDP input (Phase A above) | Protocol |
| 5 | Control API wired to layers; Settings UI + SYS_BT_* syscalls | Architect + UI |
| 6 | BLE: LE scan/connect + SMP + ATT/GATT + HOGP (Phase B above) | Protocol |

## 6. HID reports to the compositor input queue

USB HID already delivers input like this (studied in drivers/usb_hid.c):
- Mouse: `mouse_inject_hid(dx, dy, buttons, wheel)` in drivers/mouse.c.
- Keyboard: HID usage code -> PS/2 set-1 scancode (table `hid_to_set1[]`) ->
  `keyboard_process_scancode(scancode)` in cpu/isr.c, so USB and PS/2 look
  identical to every downstream consumer (the compositor input queue).

The Bluetooth HID profile funnels through the exact same two sinks. `hid.c`
declares `mouse_inject_hid` and `keyboard_process_scancode` as externs and
`bt_hid_input_report()` is the ONLY place that calls them, so Bluetooth input
is byte-identical to USB/PS2 downstream. The mouse path is wired in the
skeleton; the keyboard path is a documented TODO that should reuse (factor out)
usb_hid.c's `hid_to_set1[]` translation rather than duplicate it.

## 7. Safety and non-regression

- `g_bt_enable` defaults to 0. `bt_init()` returns `BT_ERR_DISABLED` without
  touching hardware when the flag is 0. `main.c` calls `bt_init()` inside a
  block guarded by `if (g_bt_enable)`, after preemption is enabled, alongside
  the heartbeat / net worker (a proven-good spot).
- No Bluetooth code runs on the compositor thread or in an interrupt handler;
  everything is pumped by `bt_poll()` on a dedicated worker (Phase 1+).
- Adding `bt/*.c` cannot affect any other subsystem: it is a new phase dir in
  the Makefile with no back-references.
