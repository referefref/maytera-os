# MayteraOS Changelog

A curated, public-facing history of MayteraOS development, newest first. This is
hand-written from the project's internal engineering log for a general audience:
it describes what shipped and what was verified, without internal build-server
names, network addresses, or operational detail that has no place on a public
site. For legal clarity: MayteraOS does not include, bundle, or distribute any
third-party commercial software. Where an entry mentions running software "the
user supplies" (for example, through the Win16 compatibility layer or the DOOM
engine port), that software is not part of the MayteraOS distribution.

## 2026-07-11 - On-device STL slicer groundwork (curaslice, phase 1)

Started an on-device 3D-printing slicer for MayteraOS by bringing the open-source
CuraEngine 15.04 engine (GNU AGPLv3) into the userland as a new app, curaslice.
This first phase vendors the engine's self-contained slicing core and its one
bundled dependency (the Clipper polygon library) and stands up the scaffolding
needed to compile C++ on MayteraOS for the first time: a tiny runtime shim that
maps C++ memory allocation and error handling onto the existing C library, a
lightweight text-string helper that avoids the heavy standard-library string
machinery, a fixed default print profile (0.2 mm layers, two perimeter walls,
20 percent grid infill, no support), and a build file. The network-facing pieces
of the upstream engine were replaced with inert stubs so the slicer runs purely
from a model file on disk to a g-code file on disk. Upstream engine sources were
kept byte-for-byte identical apart from a small, documented set of changes needed
to build without exceptions and without file-stream input. The engine is not yet
compiled or wired into a UI; that is the next phase. CuraEngine remains under its
own AGPLv3 license and is credited accordingly.

## 2026-07-11 - Maytera Studio: brushes, patterns, gradient shapes

Studio gained a real brush and pattern engine. Alongside the parametric round
brush it now ships a large set of stock bitmap brushes (chalk, charcoal, oils,
acrylics, sparks, foliage and more) and dozens of tileable patterns, both shown
as pickable grids in the dock. The bucket tool can fill with a pattern, and the
gradient tool grew a full set of shapes (linear, bilinear, radial, square,
conical, spiral) with repeat and reverse options, on top of the blend modes
added earlier. The bundled brush and pattern art is from the GIMP project under
its free licence.

## 2026-07-11 - On-device 3D slicing groundwork (CuraEngine)

Brought up the first C++ application in the userland: a port of the CuraEngine
15.04 slicer core (the part that turns a 3D model into printer G-code). This
required teaching the freestanding toolchain to build C++ at all (a small
runtime shim for allocation and the C++ ABI, no host runtime linked) and it now
compiles and links cleanly against the MayteraOS C library. It feeds the
existing 3D Print app's printer pipeline. On-device slicing of real models is
the next milestone.

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
gutter so multi-digit positions no longer clip; every dialog now has clickable
OK and Cancel buttons instead of relying on Enter/Esc; the Open dialog gained a
real proportional scrollbar; and the gradient tool gained a blend-mode option
(Normal, Multiply, Screen, Difference, Overlay), fixing the reported case where
a Difference gradient over layers did nothing.

Filters: six filters that shipped approximate or broken were rebuilt (HSV Noise
now really jitters hue, Emboss lighting is properly normalized, Fractal Trace
was inert in integer math and now works in fixed point, Neon gained its glow,
Bloom and Soft Glow and Drop Shadow lost their blocky halos via triple box
passes, Median Blur is much faster). Two whole new filter categories were
added: Artistic (Oilify, Cartoon, Photocopy, Apply Canvas, Glass Tile, Weave)
and Combine (Filmstrip, Contact Sheet).

Files: PNG export now uses real compression (LZ77 with a 32KB window plus
fixed-Huffman deflate) instead of uncompressed stored blocks, writes RGBA when
the image has transparency, and BMP export gained a 32-bit alpha-preserving
variant. A plan for porting the CuraEngine slicer was added to docs.

## 2026-07-11 - Maytera Studio: fix "Open with" showing a blank canvas

Opening an image with Studio from the Files "Open with" menu (or launching it
with a file path) produced a blank white canvas: the app was discarding the
file path it was launched with. Studio now opens the file it is given. Also
renamed the Files "Open with" entry from "Paint" to "Maytera Studio". A full
GIMP 3.2 parity plan for Studio was tracked internally.

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

Brought the public site up to date with the last few weeks of development,
fixed a real accessibility bug (body text with too little contrast against the
dark desktop background in a few places), corrected a stale "SMP up to 8 cores"
claim, added the MayteraOS wordmark and icon in place of a placeholder mark, and
added a dedicated section on the AI/LLM integration layer.

## 2026-07-02 - USB keyboard, mouse and boot drive can now share one controller

Fixed a real race in the USB stack where the keyboard/mouse driver and the
Mass-Storage driver could interfere with each other when both lived on the same
USB controller, which could stall the whole machine at boot. Keyboard, mouse
and a USB boot drive now coexist cleanly on one controller with no PS/2
fallback required.

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
consumer network printer on the project's own network, producing an actual
printed page for both a text job and a photo.

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

Fixed three separate, real reliability bugs in the network stack under load:
an HTTP/2 client-side read that could hang for minutes instead of timing out
cleanly; a DNS timer that was silently using the wrong tick rate, making
timeouts and cache lifetimes about 14 times too short; and a TCP
retransmit-timeout constant with the same wrong-tick-rate bug, which could
abort large transfers (like a big file read over the network) partway through.
All three are now correct and verified against multiple live hosts.

## 2026-07-01 and 2026-07-02 - AI: from a plain-English request to a running app

The AI chat assistant can now generate a small application from a plain-English
description, compile it, and launch it as a running window on the desktop, with
no code shown to the user unless they ask to see it. In a verified run, the
request "build me a tip calculator" produced a small, correctly-computing app
in a single pass. Every step is gated by the capability-token and consent
system described below. Asking the assistant to revise an app it already built,
conversationally, is designed for but not yet proven end to end.

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
calls and safe fallback stubs for the long tail, so an unfamiliar API call from
a hosted program no longer desynchronizes the emulated call stack or crashes
the interpreter. This is a general robustness milestone for the whole Win16
compatibility layer, verified across a broad range of real Windows 3.1-era
software supplied for testing (card games, puzzle games, a golf simulation, a
full-featured word processor, and other period software).

## 2026-06-29 - Verified disaster recovery after a hardware failure

The project's original build server failed permanently and did not come back
after a reboot. This turned into a real, unplanned test of the backup and
recovery process: a new build host was provisioned from a preserved source
mirror, a clean kernel build was verified compiling correctly on it, and a
test machine was rebuilt from preserved boot artifacts and confirmed booting
end to end with working networking, all on the same day as the failure.

## 2026-06-25 and 2026-06-26 - SMP: multi-core scheduling verified, and corrected

Corrected a scheduler misdiagnosis and hardened the SMP (multi-core) path,
then re-verified stable operation at 1, 2, 4, 8 and 16 cores, including
distributing real parallel kernel work evenly across every core. That testing
included runs on a host with as many as 80 physical cores available, though
each individual core count was not separately re-verified all the way up to
that ceiling. User-application scheduling across cores (beyond the boot CPU)
remains an open area of work; kernel-side work already runs in parallel
across every online core today.

## 2026-06-26 - A dockable AI chat panel, and OS-wide mouse wheel support

The AI chat assistant became a real dockable panel on the desktop instead of a
one-off window, and mouse wheel scrolling was wired through to every app
rather than a handful of special cases.

## 2026-06-24 - HTTP/2 added to the browser

The built-in web browser gained an HTTP/2 client (HPACK header compression,
stream flow control, ALPN negotiation) on top of the existing TLS 1.3 stack,
so large modern sites load fully instead of being capped by an HTTP/1.1-only
connection.

## 2026-06-23 - TLS fixed for modern sites; the browser goes live

Added support for the TLS 1.3 cipher suite most real-world CDNs actually
negotiate (AES-256-GCM with SHA-384), which had been silently blocking real
HTTPS sites from loading. Alongside it, the browser's rendering pipeline (an
adapted open-source HTML/CSS engine) went live for the first time, rendering
real pages with correct backgrounds and layout rather than plain text.

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
