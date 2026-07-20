// diskimg.h - removable disk-image mount/eject for the #257 drive layer (#196)
//
// Lets the user mount a disk IMAGE file onto a removable drive letter so that
// file access to that letter reads from the image's filesystem instead of the
// /WINDIR folder:
//   ISO9660 (.iso) and raw CD (.img)  -> E: (CD-ROM, read-only)
//   FAT12 floppy (.img / .ima)        -> A: (floppy)
//
// The image file itself lives on the normal root FS and is loaded into RAM at
// mount time (capped). Reads are served through a single hook in fat_read_file
// (diskimg_try_read), so the DOS INT 21h + Win16 file APIs transparently see the
// image. When nothing is mounted the hook returns NULL and behavior is unchanged.
#ifndef DOS_DISKIMG_H
#define DOS_DISKIMG_H

#define DISKIMG_FMT_NONE     0
#define DISKIMG_FMT_ISO9660  1
#define DISKIMG_FMT_FAT12    2

// Mount the image at native path imgpath (e.g. "/WINDIR/TEST.ISO") onto drive
// letter (A or E). Auto-detects ISO9660 vs FAT12. Returns 0 on success, negative
// on error (not found / too large / unrecognized / bad drive).
int  diskimg_mount(char letter, const char *imgpath);

// Eject: free the image and revert the drive to its folder backing.
void diskimg_eject(char letter);

// 1 if an image is currently mounted on the drive.
int  diskimg_is_mounted(char letter);

// Format of the mounted image (DISKIMG_FMT_*), or DISKIMG_FMT_NONE.
int  diskimg_format(char letter);

// Basename of the mounted image (e.g. "TEST.ISO"), or "" if none.
const char *diskimg_mounted_name(char letter);

// Read a whole file from the mounted image. relpath is relative to the image
// root, with either '/' or '\\' separators (a leading separator is ignored).
// Returns a kmalloc'd buffer (caller kfree) + *size_out, or NULL on miss.
void *diskimg_read_file(char letter, const char *relpath, unsigned int *size_out);

// Directory listing callback. is_dir: 1 directory, 0 file. size: file size bytes.
typedef void (*diskimg_dir_cb)(const char *name, int is_dir, unsigned int size,
                               void *ud);
// List the directory relpath ("" / "/" = root) of the mounted image. Returns the
// entry count, or negative on error.
int  diskimg_listdir(char letter, const char *relpath, diskimg_dir_cb cb, void *ud);

// Hook called by fat_read_file: if path is "/WINDIR/DRIVE_E/.." or
// "/WINDIR/DRIVE_A/.." AND that drive has an image mounted, serve the file from
// the image and return its kmalloc'd buffer (+*size_out); otherwise return NULL
// so the normal folder read proceeds. Safe + inert when nothing is mounted.
void *diskimg_try_read(const char *path, unsigned int *size_out);

#endif // DOS_DISKIMG_H
