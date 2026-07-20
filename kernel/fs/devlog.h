// devlog.h - #388 comprehensive boot-time device inventory to /DEVLOG.TXT
#ifndef DEVLOG_H
#define DEVLOG_H

#include "fat.h"

// Build a complete device inventory (PCI bus + full xHCI/USB device tree,
// including devices behind hubs, plus HD Audio codec identification) and write
// it to /DEVLOG.TXT on the FAT root. Also echoes a short summary to serial.
// Call once at boot after PCI + USB enumeration and after the FAT root is
// mounted/armed. Non-fatal: silently does nothing if fs is not mounted.
void devlog_dump(fat_fs_t *fs);

#endif // DEVLOG_H
