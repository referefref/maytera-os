# MayteraOS Changelog

A history of MayteraOS development, newest first.

MayteraOS does not include, bundle, or distribute any third-party commercial
software. Where an entry mentions running software through the Win16
compatibility layer or the DOOM engine port, that software is not part of the
MayteraOS distribution and must be supplied separately.

## 2026-07-20 - First public release: MayteraOS v1.95.0 (build 851)

MayteraOS v1.95.0 (build 851) is the first public release, published as a
bootable hybrid ISO. It carries a GPT partition table whose EFI System
Partition holds the whole system, so the same image can be written to a USB
stick or attached to a virtual machine as a disk. UEFI firmware loads the
bootloader, the kernel mounts its own root filesystem, and the desktop signs
in automatically with no further input needed.

The image does not boot when attached as a virtual or physical CD/DVD drive.
The kernel's ATA driver implements ATA read commands but not the ATAPI packet
interface an optical device needs, so no root filesystem is found if the image
is attached as a CD; write it to a USB stick or attach it as a disk instead.
MayteraOS's own code is released under the GNU General Public License,
version 2 or later.

## 2026-07-20 - Desktop: the Super key opens the start menu

The Super (Windows) key now opens the desktop's start menu, recognized from
both PS/2 and USB keyboards.

## 2026-07-19 - Maytera Squadron: parallax background, illustrated title art and a redesigned HUD

Maytera Squadron gained a multi-layer parallax background: a drifting nebula
wash and a far layer of planets, a ringed gas giant and an asteroid, in front
of the existing three-depth starfield, plus matching illustrated title art for
its menu and game-over screens. Flight statistics moved out of a plain overlay
and into the six readout boxes built into the side-panel artwork: score,
stage, weapon, power level, kills and stage completion on the left, and lives,
bombs, frame rate, score multiplier, accuracy and a boss/shield/hull bar on
the right. Firing is now held rather than automatic (left mouse button or
Space), and the bomb is bound to the right mouse button as well as its
existing key, with its remaining count shown in the HUD.

## 2026-07-18 to 2026-07-19 - Performance: quieter networking, faster app launches, more of the disk cached in RAM

A network circuit breaker now marks a connection faulty and stops retrying
after repeated unreachable-host failures, quieting background services
(weather, pricing, Home Assistant) instead of retrying indefinitely; it
recovers automatically the next time a connection succeeds or a new address is
applied. The 3D Print app's idle screen now redraws only when its content
actually changes, and its model preview settles after a brief reveal instead
of spinning continuously. Application binaries are stripped of debug symbols
when they are packaged onto the boot image, reducing how much each app has to
read from disk on first launch. On a USB-booted system, the RAM cache that
serves the live filesystem now covers the whole disk image, including the
application partition, not just the boot partition.

## 2026-07-18 - OS-wide text editing and a system clipboard

Standard text editing (select all, cut, copy, paste, undo, redo) and
click-drag text selection are now available across every application that
uses the shared input framework, backed by a new OS-level clipboard that lets
any two applications exchange text through the same shortcuts.

## 2026-07-17 to 2026-07-18 - A larger, faster-loading font library

The font system now enrolls each typeface by reading only its header and name
table at startup, loading a face's full outline data only the first time it
is actually used, which cut the time fonts add to startup dramatically and
made a much larger font library practical. The font store's capacity was
raised from 16 to 64 typefaces, and font names are now read from each file's
own name table rather than guessed from its filename, so families and styles
are identified and labeled correctly.

## 2026-07-18 - Maytera Studio: five new filter categories and richer filter dialogs

Maytera Studio's filter set grew by five categories in the Photoshop style:
Brush Strokes, Sketch, Stylize, Texture and Pixelate, each with its own full
options dialog, alongside parameter upgrades to several existing filters.
Filter dialogs gained an angle dial for effects with a direction, and an
editable convolution-matrix grid for custom kernel-based effects, alongside
the existing curve editor and the on-canvas positional handles used by
Lighting Effects and Lens Flare. Every filter and colour dialog also gained a
zoomable, pannable preview loupe, so a change can be checked at actual pixel
size or panned across the image before it is applied.

## 2026-07-15 to 2026-07-16 - Continued Rust migration in the kernel

Continued the incremental move of kernel components from C to originally
written Rust: checksum routines, several network and filesystem parsers (ARP,
ICMP, DNS, DHCP, URL parsing, FAT, exFAT, ext2 directory entries, ELF and PE
headers), cryptographic primitives (SHA-256, SHA-512, MD5, MD4, HMAC, AES,
ChaCha20, ed25519) and image decoders (BMP, JPEG headers, PNG's DEFLATE
decompression) now run behind the existing kernel interfaces, each verified
against its previous C implementation before replacing it. This is ongoing,
incremental work alongside the existing C kernel, not a wholesale rewrite.

## 2026-07-11 - On-device STL slicer groundwork (curaslice)

Brought the open-source CuraEngine 15.04 slicing engine (GNU AGPLv3) into the
userland as a new app, curaslice, and taught the freestanding toolchain to
build C++ for the first time: a small runtime shim maps C++ memory allocation
and error handling onto the existing C library, backed by a lightweight
text-string helper that avoids the heavy standard-library string machinery.
This first phase vendors the engine's self-contained slicing core and its one
bundled dependency (the Clipper polygon library), with a fixed default print
profile (0.2 mm layers, two perimeter walls, 20 percent grid infill, no
support). The network-facing pieces of the upstream engine were replaced with
inert stubs so the slicer runs purely from a model file on disk to a g-code
file on disk, and it links cleanly against the MayteraOS C library. Upstream
engine sources were kept byte-for-byte identical apart from a small,
documented set of changes needed to build without exceptions and without
file-stream input. CuraEngine remains under its own AGPLv3 license and is
credited accordingly.

## 2026-07-11 - Maytera Studio: brushes, patterns, gradient shapes

Studio gained a real brush and pattern engine. Alongside the parametric round
brush it now ships a large set of stock bitmap brushes (chalk, charcoal, oils,
acrylics, sparks, foliage and more) and dozens of tileable patterns, both shown
as pickable grids in the dock. The bucket tool can fill with a pattern, and the
gradient tool grew a full set of shapes (linear, bilinear, radial, square,
conical, spiral) with repeat and reverse options, on top of the blend modes
added earlier. The bundled brush and pattern art is from the GIMP project under
its free licence.

## 2026-07-11 - Maytera Studio: professional colour picker + palette system

Maytera Studio's colour tools were rebuilt into a GIMP/Photoshop-class picker.
The dock's old sixteen-swatch grid is replaced by a live mini-picker: a
saturation/value square with a vertical hue strip (both rendered from a cached
buffer that is only rebuilt when the hue changes, so painting stays responsive),
foreground and background chips with an active-target highlight, an editable
hexadecimal field, and six numeric steppers for hue/saturation/value and
red/green/blue that can be nudged or scrubbed. All colour maths is integer-only.

Double-clicking a colour chip (or the "..." affordance) opens a full "Change
Foreground/Background Color" dialog with a large square and hue strip, numeric
steppers, a hexadecimal field, a before/after split swatch whose "old" half
reverts the edit, an in-dialog eyedropper, an "add to swatches" button, a strip
of the twelve most recent colours, and OK/Cancel. The same dialog now backs the
colour parameter used by filters, so every colour choice in the app shares one
real picker.

A new Swatches dock section adds palettes: a recent-colours ring that records
every committed colour, built-in read-only "Maytera" and "Grays" palettes, and
an editable user palette that persists to disk in the standard GIMP palette text
format and reloads across reboots. Clicking a swatch sets the foreground colour,
Shift-clicking sets the background, and Alt-clicking removes a swatch from an
editable palette. The AI Palette feature now loads its generated colours into a
named, saveable palette instead of only setting the foreground.

## 2026-07-11 - Maytera Studio: Phase 1 polish, filter overhaul, real PNG compression

A large coordinated batch. Interface: the vertical ruler gained a wider labeled
gutter so multi-digit positions display cleanly; every dialog now has clickable
OK and Cancel buttons instead of relying on Enter/Esc; the Open dialog gained a
real proportional scrollbar; and the gradient tool gained a blend-mode option
(Normal, Multiply, Screen, Difference, Overlay), each applying correctly over
existing layers.

Filters: HSV Noise now jitters hue correctly, Emboss lighting is properly
normalized, Fractal Trace works in fixed point, Neon has a real glow, Bloom,
Soft Glow and Drop Shadow use triple box passes for smooth halos, and Median
Blur is much faster. Two whole new filter categories were added: Artistic
(Oilify, Cartoon, Photocopy, Apply Canvas, Glass Tile, Weave) and Combine
(Filmstrip, Contact Sheet).

Files: PNG export now uses real compression (LZ77 with a 32KB window plus
fixed-Huffman deflate) instead of uncompressed stored blocks, writes RGBA when
the image has transparency, and BMP export gained a 32-bit alpha-preserving
variant.

## 2026-07-11 - Maytera Studio opens the file it is launched with

Launching Maytera Studio from the Files "Open with" menu, or with a file path
argument, now opens that file directly on a fresh canvas. Also renamed the
Files "Open with" entry from "Paint" to "Maytera Studio".

## 2026-07-11 - Maytera Studio: many image formats, export choices, print preview

Maytera Studio can now open a much wider range of images. Alongside its own BMP
and layered .MSTU formats, it now loads PNG, JPEG, GIF and other common formats
by routing them through the system image decoder, so photos and screenshots from
anywhere open directly. Oversized images are scaled to a sensible working size
rather than being rejected.

The File menu gained clearer export choices (Export PNG and Export BMP), and a
new Print / Preview dialog that lays the flattened artwork out on a page, shows
which printer it will go to, and prints it over the network (IPP), with a
Print-to-File option when no printer is configured.

## 2026-07-02 - Public website refreshed

Brought the public site up to date with the last few weeks of development:
body text now meets standard contrast against the dark desktop background
throughout, the "SMP up to 8 cores" claim was corrected to reflect current
support, the MayteraOS wordmark and icon replaced a placeholder mark, and a
dedicated section on the AI/LLM integration layer was added.

## 2026-07-02 - USB keyboard, mouse and boot drive can now share one controller

Keyboard, mouse and a USB boot drive now coexist cleanly on one USB controller,
with no PS/2 fallback required.

## 2026-07-01 - Boots and runs entirely from a USB stick

MayteraOS can now mount its own root filesystem from a USB Mass Storage device
and boot to a fully working desktop with no internal disk present at all, no
ATA/IDE and no SATA controller. This was verified with a disk image attached
purely as USB storage: the kernel enumerated it, mounted root from it, and
booted straight through to the desktop with live widgets. It is a real step
toward running MayteraOS on plain, disk-less hardware from a single USB drive.

## 2026-07-01 - A live-USB image

Building on USB-root boot, MayteraOS can now be packaged as a single bootable
USB image carrying the full current desktop: compositor, widgets, and the
built-in apps. Verified booting end to end in a virtual machine with the image
attached as USB storage only.

## 2026-07-01 - USB Mass Storage and USB keyboard/mouse (HID) support

Added generic, spec-driven USB drivers for Mass Storage (the BBB/SCSI class
used by USB drives) and HID (the class used by USB keyboards and mice), both
running over xHCI. "Generic" matters here: the drivers read each device's own
descriptors rather than hardcoding one product, so ordinary USB storage,
keyboards and mice are expected to work rather than only a specific tested
device.

## 2026-07-01 - Network printing: documents and images to a real printer

MayteraOS can now print to a real network printer. Text documents go out over
IPP; images are sent either directly (for printers that accept JPEG natively)
or converted to PostScript for others. This was verified against a real
consumer network printer, producing an actual printed page for both a text
job and a photo.

## 2026-06-30 - Real external USB audio, from a generic driver

Added a USB Audio Class driver for external DACs (USB sound devices), written
against the USB Audio Class specification rather than one product. It was
verified working, unmodified, against two different physical USB DAC devices
from different manufacturers, negotiating each one's own sample rate and
endpoint configuration automatically. This is the foundation the music player
(below) plays audio through.

## 2026-06-30 to 2026-07-01 - A skinnable music player with EQ, album art and a visualizer

Added a full music player in the style of the skinnable MP3 "jukebox" software
of the late 1990s and early 2000s: a scrolling track display, a multi-band
graphic equalizer wired to real output gain, playlist support, embedded and
online album-art lookup, and a real-time, audio-reactive 3D visualizer. It
imports third-party skin files for the classic skin format that era's players
made popular, and decodes MP3, FLAC, Ogg Vorbis, WAV, Opus and AAC, all with
fixed-point math since the kernel has no floating-point unit dependency in that
path. Verified playing real audio files through the USB DAC driver above.

## 2026-07-01 - Kernel networking robustness pass

The network stack is reliable under sustained load: an HTTP/2 client read now
completes or times out cleanly rather than stalling, and DNS timeouts, cache
lifetimes and TCP retransmission timing all use the kernel's correct tick
rate. Large transfers, such as a big file read over the network, now complete
reliably instead of aborting partway through. Verified against multiple live
hosts.

## 2026-07-01 and 2026-07-02 - AI: from a plain-English request to a running app

The AI chat assistant can now generate a small application from a plain-English
description, compile it, and launch it as a running window on the desktop.
Generated code is not displayed unless requested. In a verified run, the
request "build me a tip calculator" produced a small, correctly-computing app
in a single pass. Every step is gated by the capability-token and consent
system described below.

## 2026-06-30 - A capability-token, consent and audit system for the AI layer

Every app and widget on MayteraOS now publishes a machine-readable tool
contract describing what it does and what it can be asked to do. The AI
assistant reads these contracts and calls into them through scoped,
time-bounded capability tokens: reading a file, writing a setting, or
compiling and deploying code are all separately gated. Higher-risk actions
require an explicit on-screen consent prompt naming exactly what is being
requested, and every grant, denial and action is written to an append-only
audit log. Unrecognized requests are denied by default rather than allowed.

## 2026-06-30 - A Device Manager app

Added a Device Manager application that inventories the system the way a
desktop OS control panel would: CPU and memory summary, the PCI device list
with vendor/device IDs and IRQs, attached USB devices, and the interrupt
table, styled to match the rest of the built-in apps.

## 2026-06-30 - SMB network share browsing, with read and write

MayteraOS can now browse, read from, and write to SMB network shares directly
from the Files app, the terminal, and other apps, using the same file APIs as
local files. Files gained a "Network" view for saved network locations, with a
dialog to add a new one. Verified end to end against a real SMB server,
including uploading a file and confirming it landed correctly on the server.

## 2026-07-01 - A from-scratch graphical installer

Added an installer that partitions a target disk, writes a real GPT partition
table and EFI System Partition, and clones the live filesystem onto it, so
MayteraOS can install itself onto a blank disk. Verified: a live instance
installed itself onto an unformatted disk, and that disk then booted
independently to the desktop with no help from the source install media.

## 2026-07-01 and 2026-07-02 - Windows 3.x-style screensaver support

The Win16 compatibility layer can now host genuine Windows 3.1-era `.SCR`
screensavers written for that platform's screensaver API, running fullscreen
and tearing down cleanly on any input, alongside MayteraOS's own native
screensaver system (Starfield, Flux, digital rain, plasma, and a couple of
OpenGL-style 3D demos). Both are wired into the same idle-timeout and
Settings-driven picker.

## 2026-07-01 - Comprehensive Win16 API ordinal coverage

Completed a pass across the entire classic Windows 3.x KERNEL, USER, GDI and
SHELL API surface used by 16-bit ("Win16") applications: essentially the full
ordinal set is now recognized, with real implementations for the commonly used
calls and safe fallback stubs for the long tail, so the interpreter stays
synchronized and stable across a broad range of real Windows 3.1-era software
(card games, puzzle games, a golf simulation, a full-featured word processor,
and other period software). This is a general robustness milestone for the
whole Win16 compatibility layer.

## 2026-06-25 and 2026-06-26 - SMP: multi-core scheduling verified

MayteraOS runs reliably across multiple cores, distributing kernel-side work
evenly from a single core up to sixteen, with further headroom on hardware
offering more cores than that. User applications are scheduled on the boot
CPU; kernel-side work runs in parallel across every online core.

## 2026-06-26 - A dockable AI chat panel, and OS-wide mouse wheel support

The AI chat assistant became a real dockable panel on the desktop instead of a
one-off window, and mouse wheel scrolling was wired through to every app
rather than a handful of special cases.

## 2026-06-24 - HTTP/2 added to the browser

The built-in web browser gained an HTTP/2 client (HPACK header compression,
stream flow control, ALPN negotiation) on top of the existing TLS 1.3 stack,
so large modern sites load fully instead of being capped by an HTTP/1.1-only
connection.

## 2026-06-23 - TLS support for modern sites; the browser goes live

Added support for the TLS 1.3 cipher suite most real-world CDNs actually
negotiate (AES-256-GCM with SHA-384), enabling real HTTPS sites to load.
Alongside it, the browser's rendering pipeline (an adapted open-source
HTML/CSS engine) went live for the first time, rendering real pages with
correct backgrounds and layout rather than plain text.

## 2026-06-23 - Desktop widgets: sticky notes and more

Added desktop sticky notes and several additional desktop widgets alongside
the existing clock, calendar and weather widgets, all persisted across
reboots.

## 2026-06-30 - A real notification system

Added a toast-and-notification-center system used across the desktop: apps
can post a notification with a severity level, it appears as a themed toast
and is retained in a notification history reachable from the system tray,
with per-severity muting and a do-not-disturb mode in Settings.

## 2026-06-30 - Wallpaper picker redesigned; Files gets a full context menu

The wallpaper picker in Settings became a live thumbnail grid instead of a
dropdown list, and the Files app's right-click menu gained working Copy, Cut,
Paste and a Properties dialog.
