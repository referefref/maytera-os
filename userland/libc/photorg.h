// SPDX-License-Identifier: MIT
// Copyright (c) MayteraOS contributors.
// Full license text: userland/libc/LICENSE (MIT License).
//
// photorg.h - photo organiser that runs under an AI escrow contract (#712).
//
// photorg_organize() is the high-level path the chatbot's device.organize tool
// (photos.organize) calls: it opens an escrow contract over a removable device,
// PLANS the reorganisation (group image files by capture date into date
// folders), DECLARES that plan as the contract's promise, EXECUTES it through
// the real capability-gated files.mkdir / files.move tools, VERIFIES the promise
// and CLOSES the contract (early on fulfilment). It never deletes anything.
//
// KERNEL-ENFORCED: see escrow.h. escrow_request enters the kernel escrow
// contract, so scope + no-delete + device are enforced at Ring 0; the promise
// check here is a userland postcondition oracle, not the enforcement boundary.
#ifndef PHOTORG_H
#define PHOTORG_H

#include "escrow.h"   // ESCROW_FULFILLED / ESCROW_PARTIAL / ESCROW_ERROR verdicts

// Organise the image files directly under `scope` (a removable device mount
// prefix such as "/MEDIA/USB") into date folders.
//
//   group_by : "YYYY" | "YYYY-MM" (default) | "YYYY-MM-DD". The folder name is
//              derived from each photo's EXIF capture date (DateTimeOriginal)
//              when the file carries one (#713), falling back to the file
//              modification time (mtime) for files with no EXIF capture date
//              (non-JPEGs, or JPEGs without the tag). See exifdate.h.
//   summary  : receives a human-readable report of what the contract promised,
//              that it was time-boxed to 1 hour and closes early on fulfilment,
//              whether the promise was kept, how many folders/moves/zero-deletes,
//              and whether it closed early. Always NUL-terminated.
//
// Returns ESCROW_FULFILLED (0), ESCROW_PARTIAL (1) or ESCROW_ERROR (-1).
int photorg_organize(const char *scope, const char *group_by, char *summary, int scap);

#endif // PHOTORG_H
