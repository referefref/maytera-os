// dospath.h - WINE-dosdevices-style drive-letter filesystem layer (#257)
//
// One host folder per DOS/Windows drive letter under /WINDIR:
//   /WINDIR/DRIVE_C  = C:\  (fixed disk, read-write)
//   /WINDIR/DRIVE_E  = E:\  (CD-ROM, read-only removable media)
//   /WINDIR/DRIVE_A  = A:\  (floppy, removable)
// A single case-normalizing translation maps drive-letter paths to native VFS
// paths and is shared by both the DOS INT 21h file I/O (dos/dosexec.c) and the
// Win16 KERNEL file APIs (exec/win16api.c). Names are uppercased to match the
// uppercase 8.3 / ext2-root convention used throughout MayteraOS.
#ifndef DOS_DOSPATH_H
#define DOS_DOSPATH_H

// (freestanding kernel: no <stdint.h>; these signatures use only int/char/void)

// Win16 GetDriveType return values. NOTE: Win16 (unlike Win32) has no DRIVE_CDROM
// constant; a CD-ROM is reported as DRIVE_REMOTE under Win 3.x / MSCDEX. The CD's
// read-only-ness is enforced at the file layer (dos_drive_writable), not the type.
#define DOS_DRIVE_UNKNOWN     0
#define DOS_DRIVE_NO_ROOT     1
#define DOS_DRIVE_REMOVABLE   2   // A: floppy
#define DOS_DRIVE_FIXED       3   // C: hard disk
#define DOS_DRIVE_REMOTE      4   // E: CD-ROM (Win16 has no CDROM type)

// Create /WINDIR + the per-drive folders (idempotent) and seed a minimal
// C:\WINDOWS + C:\WINDOWS\SYSTEM. Call once at boot after the root FS is mounted.
void dos_windir_init(void);

// Translate a DOS/Windows path to a native MayteraOS (uppercase) path.
//   "C:\\WINDOWS\\WIN.INI" -> "/WINDIR/DRIVE_C/WINDOWS/WIN.INI"
// Strict superset of the legacy "strip drive letter" behavior:
//   - a path already starting with '/'  -> passed through unchanged (uppercased)
//   - bare relative ("CHIPS.DAT")       -> reldir + "/" + name (reldir = caller
//                                          CWD / Win16 app dir; legacy behavior)
//   - no-drive absolute ("\\X")         -> "/X" on the root (legacy behavior)
//   - explicit drive ("X:\\..")         -> "/WINDIR/DRIVE_X/.." (the new mapping)
// reldir may be NULL/"" (then bare relative names resolve under the current
// drive root). out is always NUL-terminated and uppercased.
void dos_resolve_path(const char *in, const char *reldir, char *out, int outsz);

// Drive metadata. letter is case-insensitive ('a'..'z' / 'A'..'Z').
int      dos_drive_known(char letter);     // 1 if A/C/E
int      dos_drive_type(char letter);      // DOS_DRIVE_* (NO_ROOT for unknown)
int      dos_drive_writable(char letter);  // 1 for A/C, 0 for E (CD) / unknown
// Writability for a raw DOS path: looks at an explicit "X:" prefix, else the
// current drive. Native "/" paths and bare relative paths are writable (they hit
// the normal root FS). Used to reject writes/creates to the read-only CD (E:).
int      dos_path_writable(const char *in);
char     dos_current_drive(void);          // current default drive letter
void     dos_set_current_drive(char letter);
int      dos_drive_count(void);            // number of drive letters A..last (=5, A..E)

// Drive-backend hook for the disk-image mount/eject app (#196). For now every
// known drive is folder-backed under /WINDIR; #196 will register an image-backed
// reader (ISO9660 for E:, FAT12 for A:) through this hook without touching the
// translation contract above. Returns 1 if an image is currently mounted on the
// drive (always 0 until #196 wires it).
int      dos_drive_image_mounted(char letter);

#endif // DOS_DOSPATH_H
