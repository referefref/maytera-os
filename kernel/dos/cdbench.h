// cdbench.h - [no-ticket]: SEQUENTIAL READ BENCHMARK over a mounted disc image.
//
// WHY THIS EXISTS
// ---------------
// The reported defect is that a DOS game streaming from a CD image in the boot
// stick's tail stalls for 30+ seconds. The measurement that mattered was
// already available ([BLK122]) but only as a whole-boot total, mixed in with
// ext2 root traffic, app launches and the compositor. A total cannot say what a
// CD stream costs, and "run the game and time it by eye" cannot produce a
// before/after number.
//
// This is the missing instrument: it reads ONE named file off ONE mounted disc
// image, sequentially, in the same 4096-byte chunks dos/int21svc.c serves an
// INT 21h AH=3Fh with, and reports the two quantities the fix is judged on:
//
//   throughput            KB/s of file delivered
//   round trips per MB    device commands the block layer issued per megabyte
//
// The second is the mechanism and is DEVICE-INDEPENDENT: it is the same number
// in a VM and on the owner's laptop, which matters because a VM's virtual disk
// is not a USB flash drive and its MB/s figure is not transferable. The first
// is only meaningful on the hardware it was taken on, and the report says so.
//
// It is gated on /CONFIG/CDBENCH.CFG existing, so it costs one absent-file read
// on a normal boot and nothing else. It reads; it never writes.
#ifndef DOS_CDBENCH_H
#define DOS_CDBENCH_H

// Run the benchmark if /CONFIG/CDBENCH.CFG is present. Called from main.c after
// usbvol_probe_and_mount(), from a context that may block.
//
// Config format, one line:
//     <letter> <relpath> [megabytes] [chunk_bytes]
// e.g. "E /DW2/ENGLISH.SMP 32 4096"
// megabytes defaults to 32, chunk_bytes to 4096 (int21svc's io_buf size).
void cdbench_maybe_run(void);

#endif // DOS_CDBENCH_H
