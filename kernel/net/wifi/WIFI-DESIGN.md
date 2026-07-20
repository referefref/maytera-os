# MayteraOS Native WiFi Stack: Design and Phased Plan (Task #383)

Status: RESEARCH + DESIGN ONLY. No kernel build was done for this document.
Target hardware: Apple iMac14,4 (21.5-inch, Mid 2014, EMC 2805, A1418) built-in AirPort card.
Author: research pass, 2026-07-03.

---

## 0. Executive summary and honest verdict (read this first)

The single most important finding overrides the optimistic framing of the task:

> The exact WiFi chip in the iMac14,4 is a **Broadcom BCM4360** on the
> **BCM94360CD** AirPort card, PCI id most likely **14e4:43a0**. This specific
> part has **NO open-source driver on any OS**. Linux does not support it with
> `brcmfmac`; it works **only** with Broadcom's closed, binary-only `wl`
> (`broadcom-sta`) driver, which is x86-Linux-only and ships the entire MAC +
> firmware as an opaque blob. There is no public specification and no open
> reference implementation of its host/firmware protocol.

Consequences:

- The clean, documented "port brcmfmac's PCIe msgbuf fullmac protocol" plan that
  this task assumes **does not apply to the card that is actually in the iMac.**
  brcmfmac's msgbuf/flowring protocol is for BCM43602 / BCM4356 / BCM4366 and
  friends, NOT for the 4360 (14e4:43a0).
- Writing a native driver for the *as-installed* 4360 means reverse-engineering
  an undocumented proprietary firmware interface from scratch, with zero open
  reference code. That is a hardest-tier RE research project (realistically
  weeks-to-months, with a real chance it never fully works), and the firmware is
  not legally redistributable.

Therefore this document presents **three paths** and recommends a clear order:

1. **Pragmatic (recommended for connectivity now):** keep the already-working USB
   Ethernet dongle (#378, AX88772B). Native PCIe WiFi is a "for its own sake"
   research project, not the fastest route to wireless.
2. **Feasible native path (recommended if native WiFi is the actual goal):**
   physically swap the AirPort card for a **BCM943602CDP (BCM43602, 14e4:43ba)**,
   which is slot/pin-compatible in the A1418 chassis and IS a fully documented
   `brcmfmac` PCIe fullmac target with redistributable `linux-firmware` and open
   reference drivers (Linux brcmfmac, OpenBSD `bwfm`, FreeBSD). The entire design
   below is written for this target; on it the plan is genuinely achievable.
3. **Hard native path (not recommended as a first effort):** keep the 4360 and
   reverse-engineer the `wl` firmware interface. Do not start here.

Everything technical below assumes the **BCM43602 / brcmfmac** target (path 2),
because that is the only version of "native WiFi on this machine" that is real
engineering rather than open-ended reverse engineering. Section 1 explains how to
confirm the live card before committing.

---

## 1. THE CARD

### 1.1 What is in the iMac14,4

| Attribute | Value |
|---|---|
| Machine | iMac14,4, 21.5-inch Mid 2014, EMC 2805, model A1418 (MF883LL/A) |
| AirPort card | Broadcom **BCM94360CD** combo (WiFi + Bluetooth) |
| WiFi chip | **BCM4360**, PCIe, 3x3 802.11ac (a/b/g/n/ac) |
| Likely WiFi PCI id | **14e4:43a0** (BCM4360 "rev 3", the Apple variant) |
| Bluetooth chip | **BCM20702** Bluetooth 4.0, on an **internal USB** interface (NOT PCIe) |
| Class | **FullMAC** (802.11 MAC runs in on-chip firmware), but see the caveat below |

Sources: iFixit EMC 2805 teardown, Apple support 112031, EveryMac iMac14,4 specs,
replacement-part listings (BCM94360CD for A1418).

The Bluetooth side (BCM20702 over internal USB) is a separate device and is the
direct analog of the #372 Realtek-BT firmware-upload work; it is out of scope for
this WiFi document but noted in Section 8.

### 1.2 The fullmac caveat

"FullMAC" is true in the sense that the 802.11 MAC lives in firmware on the chip.
But there are two families of Broadcom fullmac firmware host-interfaces:

- The **open** family (`brcmfmac`): documented-enough msgbuf/flowring PCIe
  protocol, firmware shipped in `linux-firmware`. Chips: BCM43602, BCM4356,
  BCM4366, BCM4371, etc.
- The **closed** family (`wl` / `broadcom-sta`): the 4360's protocol. No public
  docs, driver is a binary blob. This is the one physically in the iMac.

So "BCM4360 is FULLMAC" is correct but does **not** imply "portable like
brcmfmac." That is the crux.

### 1.3 CONFIRM the live card before any work

Do not trust part databases blindly; confirm from the running machine. Options:

- Boot a Linux live USB on the iMac and run `lspci -nn | grep -i network`
  (expect `14e4:43a0` for the stock card, `14e4:43ba` if already a 43602).
- Or extend MayteraOS `drivers/pci.c` enumeration to print every
  `vendor:device:class` at boot and read the serial log. Broadcom vendor is
  `0x14E4`; a network-controller class (0x0280) entry gives the exact device id.

The user currently keeps the stick in the iMac; capture this the next time the
iMac is booted with MayteraOS or a Linux live image. The device id decides which
path (Section 0) is even possible.

---

## 2. FIRMWARE: the make-or-break gate

A Broadcom fullmac chip is inert until the host uploads a firmware image (and,
usually, a per-board NVRAM/calibration blob) into the chip's RAM over the bus.
No firmware -> no MAC -> no scan, no anything. This is exactly the #372 pattern
(upload vendor firmware before the device works), just far larger.

### 2.1 For the BCM43602 target (path 2, the real plan)

- Firmware blob: **`brcm/brcmfmac43602-pcie.bin`** from `linux-firmware`
  (git: gitlab.com/kernel-firmware/linux-firmware, `brcm/` directory).
- NVRAM: BCM43602 boards commonly carry calibration in on-chip **OTP**, so a
  separate `brcmfmac43602-pcie.txt` NVRAM file is often NOT required. If OTP is
  incomplete, a matching `.txt` is needed; the correct one may have to be
  extracted from the board or matched by board name. Treat NVRAM as a real risk
  item, second only to the upload sequence.
- Optional CLM blob (regulatory/rate tables): `brcmfmac43602-pcie.clm_blob` may
  exist; recent firmware can run without a separate CLM file.
- Licensing: `linux-firmware` ships these under `LICENCE.broadcom_bcm43xx`, which
  **permits redistribution** of the unmodified blob with the license text. So we
  can legally ship `brcmfmac43602-pcie.bin` on the MayteraOS boot disk (place it
  under e.g. `/FIRMWARE/BRCM43602.BIN`, 8.3 uppercase FAT name; keep the license
  text alongside). This is the decisive advantage of the 43602 path.

### 2.2 For the stock BCM4360 (path 3, not recommended)

- There is **no redistributable brcmfmac blob** for 14e4:43a0. Its firmware is
  embedded inside the closed `wl.ko` (extractable with `b43-fwcutter`-style
  tooling, but the *host protocol* to talk to it is undocumented and the blob is
  **not** licensed for redistribution).
- macOS carries its own firmware (AppleBCMWLANFirmware / AirPortBrcmNIC kexts).
  Extraction is a grey-area and, again, useless without the undocumented host
  protocol.
- Bottom line: on the 4360, firmware sourcing is both legally and technically a
  dead-ish end for a clean-room OS. This alone is why path 3 is not recommended.

### 2.3 Honest read on firmware difficulty

- 43602 path: **tractable.** The blob is redistributable and the upload sequence
  is described by open source. Main uncertainty is NVRAM/OTP for the specific
  board and exact firmware/driver version pairing (mismatched versions fail with
  cryptic `-2` / init errors, a very common failure mode in the wild).
- 4360 path: **effectively blocked** on both licensing and lack of protocol docs.

---

## 3. DRIVER PROTOCOL (BCM43602 PCIe fullmac, brcmfmac-modeled)

What a minimal MayteraOS driver must implement, modeled on Linux
`brcmfmac/pcie.c` + `msgbuf.c` + `commonring.c` + `flowring.c` and OpenBSD
`bwfm_pci`.

### 3.1 PCI / BAR layout

- Enable the device (bus master, memory space) via `drivers/pci.c`.
- **BAR0**: ~32 KB register window (PCIe core regs, ChipCommon, watchdog, mailbox
  / doorbell registers, interrupt mask/status).
- **BAR2**: large window mapping the dongle's **TCM** (tightly-coupled RAM /
  SoCRAM). Firmware and NVRAM are written here; the firmware-ready "shared"
  pointer is read from the top of here.
- Interrupts: MSI preferred; legacy INTx acceptable. Map an IRQ handler that
  reads/acks the D2H mailbox status.
- Note for MayteraOS: pages are UEFI identity-mapped (phys == virt) and the DMA
  descriptor rings must be on **uncached / write-through** memory or explicitly
  flushed; get this wrong and ring indices silently desync. This is the
  MayteraOS-specific footgun for the whole msgbuf layer.

### 3.2 Firmware download sequence

1. Identify chip (read ChipCommon id/rev through BAR0) and confirm 43602.
2. **Halt the internal ARM core** (Cortex-R4/M3) via ChipCommon/backplane reset
   so RAM can be written safely.
3. Copy `brcmfmac43602-pcie.bin` into TCM at the RAM base through BAR2.
4. Write the NVRAM blob near the **top** of RAM, followed by a trailing
   length + inverted-length token the firmware validates (only if OTP NVRAM is
   insufficient).
5. Write the `brcmf_pcie_shared` magic/version handshake fields.
6. **Release the ARM from reset.** Firmware boots, then writes a pointer near the
   end of TCM to its `brcmf_pcie_shared` structure.
7. Poll that shared struct for a "ready" signature; from it read `ringinfo`
   (addresses of the common rings, max flowrings, DMA index-array addresses).

This ordering is silicon-specific and unforgiving; it is the first true gate
(P1). It is well described by open source for the 43602, which is exactly why the
43602 path is viable and the 4360 path is not.

### 3.3 msgbuf ring protocol

Firmware and host communicate through fixed-size message items in DMA ring
buffers. Per ring: a producer write-index and consumer read-index (kept in a
DMA'd index array in host memory) plus a **doorbell** register (H2D mailbox) to
poke the firmware after advancing the write index.

Common rings (created by host from `ringinfo`):

- **H2D control-submit ring**: host -> firmware ioctl/iovar requests, flowring
  create/delete requests.
- **D2H control-complete ring**: ioctl responses.
- **D2H RX-complete ring** and an **event ring**: received data + async events.
- **Dynamic flowrings** (H2D): one TX ring per flow, keyed by
  (interface, destination MAC, priority). Host requests creation via a
  "flowring create" msgbuf item; firmware assigns a ring id.

Message types to implement (subset of brcmfmac's `msgbuf.h`): ioctl request,
ioctl response (BCDC-style ioctl carried inside a msgbuf item), tx-post,
rx-post (host donates empty RX buffers), rx-complete, tx-status, flowring
create/delete + response, and the firmware event message.

### 3.4 Data in/out

- **TX**: ensure a flowring exists for the flow; push a tx-post descriptor
  pointing at a DMA buffer holding the frame; advance write index; ring doorbell.
- **RX**: pre-post empty buffers on the RX-post ring; firmware DMAs frames and
  posts rx-complete items; host consumes, reframes, and re-posts buffers.
- **Frame format on the host boundary is 802.3 (Ethernet II), not 802.11.** The
  firmware does all 802.11 framing/encryption. This is what makes the NIC
  integration (Section 6) clean.

### 3.5 Minimal driver checklist

PCI BAR map + IRQ; chip identify + ARM halt/release; firmware + NVRAM upload over
BAR2; shared-struct ready handshake; DMA ring allocator + index-array + doorbell
handling for the 3 common rings; flowring create/teardown; msgbuf encode/decode
for ioctl, tx, rx, event; and the `fil`/iovar command set (Section 4/5).

---

## 4. 802.11 MLME (the minimum to join, fullmac split)

The great simplification of fullmac: the host **does not build or parse 802.11
authentication / association management frames.** The firmware runs the MLME
state machine on-chip. The host drives it with high-level commands ("ioctls" and
"iovars") over the control ring and reacts to events.

### 4.1 What the host does

- **Scan**: send the `escan` iovar (SSID list, active/passive, channel set).
  Consume `BRCMF_E_ESCAN_RESULT` events; each carries a BSS descriptor: SSID,
  BSSID, channel, RSSI, capability, and the raw **RSN IE** (which tells us the
  network's security: open / WPA2-PSK-CCMP / WPA3-SAE). Host must parse these IEs
  to render the scan list and choose crypto, but only for *display/selection*,
  not to emit frames.
- **Join (open network)**: set `wsec=0`, `wpa_auth=0` (WPA_AUTH_DISABLED), then
  issue `BRCMF_C_SET_SSID` (join) with the SSID/BSSID/channel. Firmware performs
  open-system auth + association itself. Host waits for `BRCMF_E_SET_SSID` /
  `BRCMF_E_LINK` (up) events, then data frames flow.

### 4.2 What the chip does

Beacon/probe parsing on air, the open-system auth exchange, the
assoc-request/assoc-response exchange, retries, rate/power control, power-save,
and (once keyed) CCMP encrypt/decrypt. None of this is host code for fullmac.

### 4.3 Management-frame formats the host must still understand

Only to *read*, from scan results: the IE TLV list inside beacon/probe-response
payloads (SSID, supported rates, DS-parameter channel, and the RSN IE for WPA2 /
RSNX for WPA3). We never hand-assemble auth/assoc frames. (A softmac chip would
force us to build all of these by hand; another reason the fullmac 43602 is the
right target.)

Events to handle (subset of `BRCMF_E_*`): `E_ESCAN_RESULT`, `E_SET_SSID`,
`E_LINK`, `E_ASSOC`, `E_AUTH`, `E_DISASSOC`, `E_DEAUTH`, `E_PSK_SUP`.

---

## 5. SECURITY: WPA2-PSK (and WPA3-SAE as a stretch)

### 5.1 WPA2-PSK key derivation

- **PMK** = `PBKDF2-HMAC-SHA1(passphrase, SSID, 4096 iterations, 32 bytes)`.
  Note: 802.11 uses **SHA1** here, not SHA256. `crypto/` already has HMAC-SHA256;
  we must **add SHA1 + HMAC-SHA1 + PBKDF2** (small, self-contained additions).
- **PTK** = `PRF-384/512(PMK, "Pairwise key expansion",
  min(AA,SA)||max(AA,SA)||min(ANonce,SNonce)||max(ANonce,SNonce))`, computed with
  HMAC-SHA1. Yields KCK/KEK/TK.

### 5.2 The 4-way handshake: two options with fullmac

Option (b) is strongly preferred; try it first.

- **(a) Host-side supplicant.** Set `wsec=AES_ENABLED`, `wpa_auth=WPA2_PSK`, join.
  Firmware associates (open auth); then EAPOL-Key frames arrive as **ordinary
  data frames** on the RX path. Host: verify MIC on msg1/msg3 (HMAC-SHA1, or
  AES-CMAC for the CCMP AKM), derive PTK, unwrap the GTK KDE (AES key-wrap),
  build msg2/msg4, and install PTK/GTK into firmware via the **`wsec_key`**
  iovar. Firmware then does CCMP in hardware. This is a compact software
  supplicant: PBKDF2 + HMAC-SHA1 + AES-CMAC + AES key-unwrap, all buildable on
  `crypto/`'s AES.
- **(b) Firmware handshake offload.** Push the PMK into firmware with
  **`BRCMF_C_SET_WSEC_PMK`**; if the firmware advertises the PSK-offload feature
  (`E_PSK_SUP` / internal supplicant "idsup"), the chip runs the entire 4-way
  itself. Host just sets crypto iovars + PMK, joins, and waits for `E_PSK_SUP`
  success + `E_LINK` up. Far less host code and no EAPOL timing to get right.
  Whether 43602-era firmware exposes the offload must be probed at runtime
  (feature flag in the firmware caps string).

Recommendation: implement PMK derivation + `SET_WSEC_PMK` offload (b) first;
keep the host 4-way (a) as fallback if the firmware lacks the offload.

### 5.3 CCMP / AES-128 data path

Once keys are installed, **the chip encrypts/decrypts every data frame in
hardware.** The host never CCMP-encrypts payloads. Host AES is needed only for
the handshake KDE handling (GTK key-unwrap) and the AES-CMAC MIC in the CCMP-AKM
case. Reuse `crypto/` AES; no new bulk-crypto data path.

### 5.4 WPA3-SAE (stretch, out of scope for first passes)

SAE (dragonfly) is a host-side commit/confirm exchange over group 19 (NIST
P-256) or an FFC group, needing constant-time modular exponentiation or P-256 EC
math that MayteraOS does not have. Some newer firmware offloads SAE, but 43602
vintage likely does not. Mark WPA3 explicitly out of scope until WPA2 works and
a P-256 primitive exists. Most real networks still accept WPA2, so this is not a
blocker for usable WiFi.

---

## 6. INTEGRATION into MayteraOS

### 6.1 As a NIC in net.c

Once associated, a fullmac card is just an Ethernet-like NIC handing 802.3
frames. Wrap it exactly like `e1000` / `virtio-net` / `usb_asix`:

- Implement the NIC ops the existing stack expects: `send(frame,len)`,
  RX delivery into `eth_receive()`, `link_up()` (cheap cached read; follow the
  #381 rule: never do a blocking device round-trip on the caller/UI thread),
  and `mac_addr()`.
- Register via `net_register_nic()` so ARP/IP/UDP/TCP/DHCP/DNS/TLS all reuse the
  existing stack with zero changes.
- **Link-up definition**: NIC reports up only after association AND (for secured
  networks) the 4-way handshake completes and keys are installed. Before that,
  `link_up()` returns false so the #381 `net_worker` does not start DHCP
  prematurely. When link comes up, the existing edge-triggered DHCP path (#381)
  runs unmodified.
- **WiFi-specific control (scan/join/set-key) is a side-band API**, NOT part of
  `nic_ops`. Keep `net/wifi/` exposing `wifi_scan()`, `wifi_connect(ssid,psk)`,
  `wifi_status()`; the generic NIC layer stays WiFi-agnostic.

### 6.2 Settings "WiFi" tab (gui)

Model on the existing Settings app tabs. Provide:

- A scan list: SSID, RSSI (bars), security badge (open / WPA2 / WPA3), refresh.
- Select network -> passphrase entry (reuse the on-screen keyboard / text field).
- Connect -> show state machine: Scanning / Associating / Handshaking /
  Connected (with IP once DHCP binds) / Failed(reason from `E_*`).
- Persist known networks (SSID + PSK) to a FAT file (e.g. `/WIFI.CFG`) so the
  #381 `net_worker` can auto-reconnect on boot / carrier return. Store PSK
  hashed-to-PMK if possible to avoid keeping the plaintext passphrase.

The GUI talks to the side-band `wifi_*` API; it does not touch msgbuf directly.

---

## 7. PHASED PLAN with realistic difficulty

All phases assume the **BCM43602** target (path 2). Difficulty ratings are honest.

| Phase | Goal | Difficulty | Notes / hardest sub-gates |
|---|---|---|---|
| **P0 Recon** | Confirm live PCI id on the iMac; decide card path | Trivial but **decisive** | If it reads `14e4:43a0`, either swap to a 43602 card or accept path 3 (RE). Do not proceed on 43a0 expecting brcmfmac to work. |
| **P1 PCIe bring-up + firmware load** | Map BARs, chip id, ARM halt/release, upload fw(+nvram) over BAR2, shared-struct ready | **HARD** | First true gate. Silicon-specific reset ordering; NVRAM/OTP correctness; firmware/driver version pairing. Uncached DMA memory on MayteraOS. |
| **P2 msgbuf rings + ioctl/event** | DMA ring alloc, index arrays, doorbells; round-trip one ioctl (get "ver"/MAC) + receive one event | **HARD** | DMA coherency vs identity-mapped pages; doorbell/index semantics; IRQ vs poll. If P1+P2 work, the rest is "normal" driver work. |
| **P3 Scan + associate (open)** | escan -> parse `E_ESCAN_RESULT` -> `SET_SSID` join open AP -> `E_LINK` up -> pass 802.3 frames | **MEDIUM** | Mostly command/event plumbing once P2 is solid. First "it connects" milestone. |
| **P4 WPA2 4-way + CCMP** | PBKDF2/SHA1; try PMK offload (`SET_WSEC_PMK`), else host 4-way (PTK/MIC/`wsec_key`) | **MEDIUM-HARD** | Add SHA1/HMAC-SHA1/PBKDF2 + AES-CMAC + key-unwrap. EAPOL timing/retries if offload absent. |
| **P5 NIC + DHCP + UI** | Wrap as net.c NIC, hook #381 net_worker DHCP, Settings WiFi tab, persist networks | **LOW-MEDIUM** | Mirrors e1000/usb_asix. Lowest-risk phase; do last. |

### 7.1 The hardest gates, ranked

1. **Firmware sourcing/legality for the actual card (P0/P1).** On 43a0 this
   blocks the whole project; on 43602 it is solved by redistributable
   linux-firmware. This is why P0 is decisive.
2. **P1 firmware upload sequence.** No open reference exists for 4360; a real,
   readable reference exists for 43602. Reset ordering + NVRAM are the pitfalls.
3. **P2 msgbuf DMA/doorbell correctness** on MayteraOS's identity-mapped,
   cache-managed memory.
4. **P4 WPA2 timing** if the firmware supplicant offload is unavailable.

### 7.2 Honest read on achievability

- **On the as-installed 4360 (14e4:43a0): not realistically achievable** as a
  clean project. No open driver, no spec, non-redistributable firmware. It would
  be a months-long reverse-engineering effort with a real chance of never fully
  working. Do not start here.
- **On a swapped-in BCM43602 (14e4:43ba): achievable but ambitious.** With open
  reference drivers (Linux brcmfmac, OpenBSD `bwfm`) and redistributable
  firmware, P1-P5 is a genuine port. Expect several weeks of focused work
  concentrated in P1+P2; P3-P5 are comparatively routine. This is the path to
  recommend if native WiFi is the goal.
- **Pragmatic alternative (recommended default):** the USB Ethernet dongle
  (#378) already works end-to-end (DHCP/DNS/TLS/HTTP verified). For wireless
  specifically, a **USB WiFi dongle** with a documented or well-RE'd chip is a
  far smaller effort than native PCIe Broadcom, and reuses the existing USB
  plumbing. Native PCIe WiFi on this iMac is a "someday, for the challenge"
  project, not the fast path to connectivity.

---

## 8. Relation to #372 (Realtek Bluetooth firmware upload)

WiFi here is the **same class of problem** as #372: a device that does nothing
until the host uploads a vendor firmware blob over the bus before any protocol
works. Reuse the mental model and any scaffolding from #372:

- blob shipped on the FAT boot disk (8.3 uppercase name), chunked upload to the
  device, and a post-upload "ready" poll before speaking the real protocol.

But scope honestly: #372 uploads one blob and then speaks a documented HCI
protocol. WiFi (even on the friendly 43602) adds a full DMA **msgbuf/flowring
ring protocol**, an 802.11 MLME command/event surface, a WPA2 supplicant, and
firmware-licensing constraints on top. It is #372's pattern at perhaps 100x the
surface area. And the iMac's Bluetooth (BCM20702 over internal USB) is itself a
#372-style firmware-upload device, separate from this WiFi work.

---

## 9. File/tree plan (when/if implemented)

Proposed layout under `net/wifi/` (picked up by the kernel Makefile's
`$(wildcard net/wifi/*.c)` once added):

- `wifi.c/.h` - side-band API (`wifi_scan`, `wifi_connect`, `wifi_status`) + NIC
  registration into `net.c`.
- `brcmfmac_pcie.c` - PCI bring-up, BAR map, firmware/NVRAM upload, shared-struct
  handshake, IRQ.
- `brcmfmac_msgbuf.c` - ring allocator, index/doorbell handling, ioctl/tx/rx/event
  encode-decode, flowrings.
- `brcmfmac_fil.c` - iovar/ioctl command layer + event dispatch (scan/join/keys).
- `wpa2.c` - PBKDF2-SHA1, PTK derivation, 4-way handshake (host fallback),
  key-unwrap; leans on `crypto/` AES + a new SHA1/HMAC-SHA1.
- Settings WiFi tab lives in `gui/settings.c` (side-band API only).
- Firmware blob on the boot disk, e.g. `/FIRMWARE/BRCM43602.BIN` (+ license text).

---

## 10. Key sources

- iFixit iMac 21.5" EMC 2805 (Mid 2014) teardown/device page.
- Apple support 112031 (iMac 21.5-inch Mid 2014 tech specs); EveryMac iMac14,4.
- Replacement-part listings identifying BCM94360CD / BCM943602CDP for A1418.
- Linux Wireless brcm80211 driver docs (brcmfmac PCIe supported chips; 43602 vs
  4360): wireless.docs.kernel.org.
- Ubuntu bug 2139532 and Arch/Debian wikis: BCM4360 14e4:43a0 needs proprietary
  `wl`/`broadcom-sta`, not brcmfmac.
- linux-firmware `brcm/` (gitlab.com/kernel-firmware/linux-firmware) and
  `LICENCE.broadcom_bcm43xx` (redistribution terms).
- Linux `brcmfmac/pcie.c`, `msgbuf.c`, `commonring.c`, `flowring.c`; OpenBSD
  `bwfm_pci` as clean-room references for the 43602 msgbuf protocol.
- linux-wireless "brcmfmac: support 4-way handshake offloading for WPA/WPA2-PSK"
  (BRCMF_C_SET_WSEC_PMK, E_PSK_SUP).
