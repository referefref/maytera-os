// kbridge.h - the wall between the two type universes (#DOSRING3).
//
// THE PROBLEM THIS SOLVES. The DOS sources are compiled against the KERNEL's
// headers (kernel/types.h typedefs `size_t` as `unsigned long long` and `bool`
// as `int`); the Ring-3 implementation must speak libc. Those two header
// worlds define overlapping names incompatibly, so no single translation unit
// can include both.
//
// THE WALL. kshim.c is compiled in the KERNEL universe and implements the ~100
// symbols the DOS sources import. ushim.c is compiled in the LIBC universe and
// implements the functions below. They meet ONLY through this header, whose
// every signature uses primitive C types that are ABI-identical in both
// universes (long, unsigned long long, int, char *, void *). No struct, no
// typedef, no enum crosses the wall - so the two worlds never have to agree on
// anything a header could get subtly wrong.
#ifndef DOSRING3_KBRIDGE_H
#define DOSRING3_KBRIDGE_H

// ---- diagnostics ---------------------------------------------------------
void kb_log(const char *bytes, unsigned long len);
void kb_abort(const char *msg) __attribute__((noreturn));

// ---- memory --------------------------------------------------------------
void *kb_malloc(unsigned long n);
void  kb_free(void *p);

// ---- filesystem (the ONE seam: MEASURED 77 call sites, all through the FAT
// API against g_fat_fs, which on the two-partition image is already the
// FAT-or-ext2 routing layer. In Ring 3 the router is the kernel's own open(),
// which routes by path exactly the same way AND applies this process's real
// credentials to every access - see kshim.c's guestfs note.) ---------------
#define KB_O_READ   0
#define KB_O_WRITE  1
#define KB_O_RDWR   2
long kb_open(const char *path, int mode);       // >=0 fd, <0 error
long kb_read(long fd, void *buf, unsigned long n);
long kb_write(long fd, const void *buf, unsigned long n);
long kb_seek(long fd, long off, int whence);    // whence: 0 set, 1 cur, 2 end
void kb_close(long fd);
long kb_size(const char *path);                 // <0 if absent

// ---- #VOLAPI: the mediated volume gateway --------------------------------
// The ONE thing the Ring-3 DOS host cannot get from open()/read(): the answer
// to "what disc is this". Everything else about a mounted volume it reaches as
// ordinary files beneath the root this returns.
//
// A `void *` and a size, rather than a struct, because a struct may not cross
// the wall. Both sides name it dimg_vol_t (kernel/dos/diskimg.h in the kernel
// universe, userland/libc/syscall.h in the libc one) and both are already
// _Static_asserted to 288 bytes, so the size argument is a THIRD check on a
// width that two asserts already hold rather than a hopeful convention.
//
// Returns 0 and fills out288; -1 bad letter or wrong size; -13 this process's
// credentials may not traverse that volume; -14 bad pointer.
int kb_volinfo(int letter, void *out288, unsigned long size);
int  kb_exists(const char *path);
int  kb_isdir(const char *path);
int  kb_mkdir(const char *path);
int  kb_unlink(const char *path);
int  kb_rename(const char *from, const char *to);
// Directory enumeration. kb_opendir returns an opaque handle (0 = failure).
unsigned long kb_opendir(const char *path);
int  kb_readdir(unsigned long h, char *name_out, unsigned long cap, int *is_dir);
void kb_closedir(unsigned long h);

// ---- the screen's own progress (#flipfix) --------------------------------
// The kernel's monotonic count of framebuffer presents (gui/fb_syscall.c
// g_fb_flip_count), through SYS_FB_FLIP_COUNT. NOT a count kept here: a second
// counter of the same event is the fork the reuse rule forbids, and this host's
// whole defect was a local copy of this symbol that nothing ever wrote.
// Returns 0 if the running kernel has no such call, which the caller must
// treat as "cannot tell", never as "the screen is not moving".
unsigned long long kb_fb_flips(void);

// ---- time ----------------------------------------------------------------
// kb_kernel_mono_us is the KERNEL's TSC-backed monotonic clock (SYS_MONO_US),
// i.e. the very clock cpu/mono.h exposes in Ring 0, so the Ring-3 host reports
// times on the same clock as the in-kernel path rather than on a second one of
// its own. kb_mono_us is CLOCK_MONOTONIC through libc and is what the DOS
// layer's own scheduling already uses; the two are separate entries because
// they can legitimately differ in epoch, and silently substituting one for the
// other is how an instrument comes to disagree with the thing it measures.
unsigned long long kb_kernel_mono_us(void);
unsigned long long kb_mono_ms(void);
unsigned long long kb_mono_us(void);
unsigned long long kb_realtime_us(void);
void kb_localtime(int *h, int *m, int *s, int *day, int *mon, int *year, int *wday);
void kb_sleep_ms(unsigned int ms);
void kb_yield(void);

// ---- audio (#181 Ring-3 audio) -------------------------------------------
// The Ring-3 PCM sink, reached through SYS_AUDIO_PCM_OPEN/WRITE/CLOSE/CTL.
// Primitive types only, per the wall: the frame buffer crosses as void * and
// is this process's own memory either side, so no struct and no typedef has to
// agree across the two universes.
//
// kb_pcm_write BLOCKS while the sink ring is full, exactly as the in-kernel
// audio_pcm_write_kernel() does, and that block IS the pacing for a real-time
// producer. It is only ever called from the dedicated SB pump THREAD, never
// from the interpreter thread, so it cannot stall the guest.
long kb_pcm_open(unsigned rate, unsigned channels, unsigned format);
long kb_pcm_write(long handle, const void *pcm, unsigned frames);
long kb_pcm_close(long handle);
// The counters and the two #426 waits, one named function each rather than a
// generic ctl(op,...): an op NUMBER crossing the wall would be a constant the
// two universes have to agree about, which is exactly what this header exists
// to avoid. The numbers stay on the libc side, in ushim.c, next to the syscall.
long kb_pcm_consumed(long handle);
long kb_pcm_wait_below(long handle, unsigned max_used, unsigned ms);
long kb_pcm_wait_consumed(long handle, unsigned target, unsigned ms);
// 1 when the kernel reports a real output device (codec or USB DAC). Cached
// for one second: it is asked on emulated-port reads, which a guest polls hot.
int  kb_pcm_avail(void);
// The capture tap: see ushim.c. Returns bytes written, or <0 when off.
long kb_pcm_tap(const void *pcm, unsigned frames);


// ---- the FM bridge (#fmbridge) -------------------------------------------
// The kernel's ONE OPL2 event queue (kernel/dos/dosfmq.c), through
// SYS_DOS_FM_HOST. NOT a queue kept here: this host used to have one of its
// own, filled correctly by the guest and drained by nobody, because
// /APPS/FMSYNTH drains the KERNEL's queue. That is the defect these entries
// exist to close, so a second queue in this process is the one thing they must
// never become.
//
// ONE NAMED FUNCTION PER OPERATION, not a generic fm_ctl(op, ...), for the
// reason the PCM block above gives: an op NUMBER crossing this wall would be a
// constant the two header universes have to agree about, which is exactly what
// this header exists to avoid. The numbers stay on the libc side, in ushim.c,
// beside the syscall.
//
// kb_fm_open returns 0 on success and <0 if the kernel refused (an in-kernel
// DOS guest already holds the queue, or the syscall does not exist). The caller
// must treat a refusal as "no FM", never as success: only the process that
// opened the queue may push to it.
int  kb_fm_open(void);
void kb_fm_push(unsigned reg, unsigned val, unsigned long long t_us);
int  kb_fm_close(void);
// The four counters and the capacity, each a plain non-negative count, or <0
// if this process does not own the queue.
long kb_fm_pushed(void);
long kb_fm_dropped(void);
long kb_fm_peak(void);
long kb_fm_used(void);
long kb_fm_capacity(void);
// rustkern/fmq.rs's own self-test, run by the kernel against the real queue and
// leaving it open. Returns the number of failing checks, or <0.
int  kb_fm_selftest(void);
// Ask the kernel to start /APPS/FMSYNTH. Returns its pid, or <=0. The kernel
// runs its own fm_launch_synth() (gui/desktop.c), including the refusal to
// launch on a machine with no audio sink, so there is ONE launcher with ONE set
// of preconditions rather than two that can drift.
int  kb_fm_launch(void);

// ---- threads and blocking ------------------------------------------------
int   kb_thread_start(void (*entry)(void *), void *arg, const char *name);
void *kb_mutex_new(void);
void  kb_mutex_lock(void *m);
void  kb_mutex_unlock(void *m);
void *kb_cond_new(void);
// Blocks until kb_cond_broadcast, or until absolute deadline_ms (0 = never).
// Returns 0 if woken, 1 if the deadline passed. The caller MUST hold `m`.
int   kb_cond_wait(void *c, void *m, unsigned long long deadline_ms);
void  kb_cond_broadcast(void *c);

// ---- host window (the present seam; same one DOOM uses) ------------------
int   kb_win_create(const char *title, int x, int y, int w, int h);
void  kb_win_destroy(int handle);
// Blit an ARGB buffer of w*h pixels to (x,y) in the window's content area.
void  kb_win_blit(int handle, int x, int y, int w, int h, const void *argb);
void  kb_win_invalidate(int handle);
int   kb_win_focused(int handle);
int   kb_win_content_size(int handle, int *w, int *h);
int   kb_win_work_area(int *x, int *y, int *w, int *h);

// ---- input ---------------------------------------------------------------
// Raw set-1 make/break scancode bytes for the guest's INT 9. Returns -1 when
// the queue is empty. This is the Stage-1 syscall (see kshim.c).
int   kb_scancode_get(void);
void  kb_scancode_clear(void);
void  kb_scancode_tap(int on);
unsigned int kb_key_modifiers(void);
void  kb_mouse_state(int *x, int *y, unsigned int *buttons);

// ---- identity ------------------------------------------------------------
unsigned int kb_uid(void);
unsigned int kb_gid(void);
// The session user's home directory, no trailing '/'. Returns 0 on success,
// -1 if it will not fit or there is no usable home. This is libc's
// userhome_root() (userconf.c), THE home lookup in the tree, not a second one:
// the DOS layer expands the "%HOME%" token in a guest path with it, and a
// second definition of where home is would aim a guest's save files somewhere
// the rest of the system does not agree with.
int kb_home(char *out, unsigned long cap);

// ---- the window event pump (drives input + close, runs on its own thread) -
void kb_pump_note(long pass, long got, long last);
void kb_pump_start(int win_handle);

#endif // DOSRING3_KBRIDGE_H
