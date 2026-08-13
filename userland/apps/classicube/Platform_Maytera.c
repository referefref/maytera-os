/*
Platform_Maytera.c - the MayteraOS backend for ClassiCube's Platform_* contract.

MayteraOS ClassiCube port (#800; task 28 in the port queue). THIS FILE IS
MAYTERA-AUTHORED and lives at the top of userland/apps/classicube/, alongside
the other Maytera backends. Everything under vendor/ClassiCube/ is vendored
upstream source, byte-identical to the pin; see PROVENANCE.md.

Written against src/Platform_Posix.c as the reference implementation, but this
is NOT a POSIX port: CC_BUILD_POSIX stays undefined (see the PLAT_MAYTERA branch
that engine-patches/coreh-maytera.py adds to the STAGED Core.h) because the
MayteraOS libc has no <sys/socket.h>, <netdb.h>, <dlfcn.h>, <signal.h>-with-
sigaction, or <utime.h>. Where a POSIX facility genuinely exists in
userland/libc it is used directly; where it does not, the function is either
implemented over a MayteraOS syscall or left to the CC_NO_* stub blocks in
_PlatformBase.h.

=============================================================================
STUB INVENTORY - what is NOT implemented, and exactly what that costs.
Keep this list correct. A stub that is not listed here is a lie in the build.

  Socket_* are NOT stubbed. Socket_Maytera.c implements the whole contract on
  the real BSD socket syscalls (userland/libc/sys/socket.h), and Http_Maytera.c
  implements the HTTP contract on the kernel's async fetcher, so
  CC_BUILD_NETWORKING stays DEFINED and multiplayer is in scope. This file must
  therefore NOT define CC_NO_SOCKETS: doing so makes _PlatformBase.h emit its
  own Socket_* definitions and the build fails with a redefinition of
  Socket_ParseAddress. (Merged from lane agent/cc-net; the text that used to be
  here described the pre-merge single-lane state.)

  Updater_Clean / _Start / _GetBuildTime / _MarkExecutable /
  _SetNewBuildTime, Updater_Supported                       [CC_NO_UPDATER]
      -> The launcher's update screen reports that updating is unsupported.
         Correct for MayteraOS: apps ship in the golden image / App Store.

  DynamicLib_Load2 / _Get2 / _DescribeError                  [CC_NO_DYNLIB]
      -> ClassiCube PLUGINS cannot load. CC_BUILD_PLUGINS is undefined too, so
         nothing tries. Not fixable without a userland dynamic loader.

  CrashHandler_Install / _DumpRegisters                 [CC_NO_CRASHHANDLER]
      -> A segfault produces NO register dump and NO faulting-instruction
         backtrace in client.log. You get the kernel's serial panic instead;
         Platform_Init logs the runtime address of Process_Exit so a faulting
         RIP can still be turned into a source line with addr2line.
         TO IMPLEMENT: userland/libc/signal.h exists, but a useful handler
         needs the trap frame, which this signal layer does not hand to Ring 3.

  Platform_Encrypt / _Decrypt / _GetEntropy              [CC_NO_ENCRYPTION]
      -> The launcher cannot save an encrypted password, so the sign-in
         password is simply not remembered. Two independent reasons, both
         real: there is no stable machine ID to derive an XTEA key from, and
         there is no CSPRNG exposed to Ring 3 at all (no /dev/urandom, no
         getrandom, no SYS_* entropy call). ERR_NOT_SUPPORTED is the honest
         answer; mixing clocks and pointer values into something that LOOKS
         like entropy would be worse than having none, because a caller
         cannot tell the difference.

  Process_StartOpen, Process_OpenSupported                      [CC_NO_OPEN]
      -> "Open the ClassiCube website" / "open this folder" do nothing.
         TO IMPLEMENT: sys_spawn_args() onto the MayteraOS browser app; needs
         the installed binary name CONFIRMED first, not guessed.

  Directory_GetCachePath                               [empty, deliberately]
      -> Same as every desktop platform upstream: there is no separate cache
         directory, everything lives under the working directory.

  DateTime_CurrentLocal returns UTC, not local time.
      -> Chat log and screenshot timestamps are UTC. MayteraOS has no timezone
         database and userland/libc/time.c defines localtime_r as a straight
         alias of gmtime_r.

  Platform_LoadSysFonts walks /FONTS, but SysFonts_Register is itself the
  not-supported stub while CC_BUILD_FREETYPE is undefined.
      -> ClassiCube renders with its own built-in bitmap font; no system font
         selection in the options menu.
=============================================================================

Copyright 2014-2025 ClassiCube (the contract this implements) | BSD-3.
The MayteraOS implementation below is part of MayteraOS.
*/
#include "Core.h"
#if defined CC_BUILD_MAYTERA

/* ---------------------------------------------------------------------------
 * Which generic fallbacks in _PlatformBase.h this platform opts into.
 *
 * Each of these REPLACES a block of code this file would otherwise have to
 * provide, with an honest "not supported" implementation. They are declared
 * here, together, so the set of things MayteraOS cannot do is one readable
 * list rather than something you have to infer from missing functions.
 * ------------------------------------------------------------------------- */
/* NOT defined: CC_NO_SOCKETS. Socket_Maytera.c supplies the real Socket_*
   implementation over userland/libc/sys/socket.h, so the generic stubs in
   _PlatformBase.h must stay out of this translation unit. */
/* No self-update: MayteraOS ships apps through the golden image / App Store. */
#define CC_NO_UPDATER
/* No dynamic loader. User ELFs are a single RWX PT_LOAD with kernel-applied
   relocations (see userland/user-pie.ld); there is no dlopen at all. */
#define CC_NO_DYNLIB
/* No sigaction/ucontext, so no faulting-instruction backtrace. Process_Abort2
   still logs and terminates through Logger_DoAbort. */
#define CC_NO_CRASHHANDLER
/* No machine ID to key XTEA off, and no CSPRNG exposed to Ring 3, so
   Platform_Encrypt/Decrypt/GetEntropy all report ERR_NOT_SUPPORTED rather than
   pretending. Consequence: the launcher cannot save an encrypted password. */
#define CC_NO_ENCRYPTION
/* No "open this URL/file in the system handler" yet. */
#define CC_NO_OPEN
/* Use _PlatformBase.h's generic argv parser. */
#define DEFAULT_COMMANDLINE_FUNC

#include "Platform.h"
#include "String_.h"
#include "Funcs.h"
#include "Errors.h"
#include "Utils.h"
#include "Constants.h"
#include "SystemFonts.h"
#include "Window.h"

/* MayteraOS libc */
#include "stdlib.h"
#include "string.h"
#include "stdio.h"
#include "unistd.h"
#include "errno.h"
#include "time.h"
#include "dirent.h"
#include "pthread.h"
#include "syscall.h"
#include "sys/stat.h"

/* ---------------------------------------------------------------------------
 * Where the game keeps its data (texpacks, maps, options.txt, client.log).
 *
 * ClassiCube is written around a CURRENT WORKING DIRECTORY, not an absolute
 * data root: every path it opens is relative ("texpacks/default.zip"). So the
 * whole story is "chdir here once at startup", which
 * Platform_SetDefaultCurrentDirectory does. Override at build time with
 * -DCC_MAYTERA_DATA_DIR=\"/somewhere\".
 *
 * /GAMES is on the ext2 root partition, which is the writable one; the FAT ESP
 * holds boot assets only (see the invariant gate in build/).
 *
 * IT MUST NOT BE UNDER /APPS. build/build-golden.sh installs the built ELF as
 * /APPS/<binary basename>, and this binary is CLASSICUBE, so "/APPS/CLASSICUBE"
 * names the EXECUTABLE. Using it as the data directory was measured on a booted
 * system: mkdir returned EEXIST (the file was already there), chdir failed with
 * "Error 1 when setting current directory", and the game then created texpacks/
 * and audio/ in its inherited working directory, the FILESYSTEM ROOT. /GAMES is
 * where game data already lives here (/GAMES/DOOM alongside DOOM1.WAD,
 * /GAMES/KEEN4), and it cannot collide with an /APPS binary name.
 * ------------------------------------------------------------------------- */
#ifndef CC_MAYTERA_DATA_DIR
#define CC_MAYTERA_DATA_DIR "/GAMES/CLASSICUBE"
#endif

/* MayteraOS's OS-wide TrueType store, walked by Platform_LoadSysFonts. Matches
   FONTS_DIR in userland/libc/gui_font.c - kept as an independent constant on
   purpose, the same convention the rest of userland uses for kernel-shared
   paths (no header crosses from libc internals into a vendored app). */
#define CC_MAYTERA_FONTS_DIR "/FONTS"

const char* Platform_AppNameSuffix = " maytera";

/* MayteraOS runs the launcher and the game in ONE process. There is a
   SYS_SPAWN_ARGS, but re-launching ourselves would mean the compositor tears
   down and recreates the window on every launcher <-> game transition, and the
   game would lose the window it is already drawing into. Same choice every
   console port makes. */
cc_uint8 Platform_Flags = PLAT_FLAG_SINGLE_PROCESS;
cc_bool  Platform_ReadonlyFilesystem;

/* errno values as returned by userland/libc/errno.h. These are the values the
   libc wrappers actually set (kernel returns -errno, the wrapper negates it),
   NOT guesses: ENOENT 2, EEXIST 17. ReturnCode_PathNotFound has no distinct
   errno here (MayteraOS reports a missing directory component as ENOENT too),
   so it gets the same out-of-band sentinel Platform_Posix.c uses, which keeps
   ReturnCode_IsNotFound() a two-value test instead of collapsing to one. */
const cc_result ReturnCode_FileShareViolation = 1000000000; /* not used */
const cc_result ReturnCode_FileNotFound       = ENOENT;
const cc_result ReturnCode_PathNotFound       = 99999;
const cc_result ReturnCode_DirectoryExists    = EEXIST;

/* WHY CC_BUILD_NETWORKING IS UNDEFINED FOR THIS ONE INCLUDE, AND ONLY IT.
   _PlatformBase.h:379-441 is a single #if defined CC_BUILD_NETWORKING block
   that emits the generic Socket_ParseAddress and expects THIS translation unit
   to supply static ParseIPv4 / ParseIPv6 / ParseHost, the way Platform_Posix.c
   does. MayteraOS instead supplies the whole Socket_ParseAddress from
   Socket_Maytera.c, because resolving a name here means driving the async
   SYS_DNS_START / SYS_DNS_POLL pair. Leaving the macro defined gives
   "redefinition of Socket_ParseAddress" plus undefined ParseIPv4/6/Host.
   That block is the ONLY user of the macro in this header (grep it), so this
   undef removes exactly it and nothing else. It is restored immediately, since
   every other translation unit and the rest of this one need it defined. */
#undef  CC_BUILD_NETWORKING
#include "_PlatformBase.h"
#define CC_BUILD_NETWORKING


/*########################################################################################################################*
*------------------------------------------------------Logging/Time-------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Log(const char* msg, int len) {
	/* write() is the libc wrapper over SYS_WRITE. Deliberately not checked:
	   there is nothing useful to do if logging fails, and a return-value check
	   here is the one place a logger must not recurse. */
	(void)!write(STDOUT_FILENO, msg, (size_t)len);
	(void)!write(STDOUT_FILENO, "\n", 1);
}

TimeMS DateTime_CurrentUTC(void) {
	/* time() is seconds-resolution wall clock (SYS_TIME). ClassiCube counts
	   from 1/1/0001, unix counts from 1/1/1970. */
	return (cc_uint64)time(NULL) + UNIX_EPOCH_SECONDS;
}

void DateTime_CurrentLocal(struct cc_datetime* t) {
	struct tm loc;
	time_t s = time(NULL);

	/* MayteraOS has no timezone database: userland/libc/time.c defines
	   localtime_r as a straight alias of gmtime_r, so "local" is UTC. That is
	   a real limitation, not an oversight - it only affects the timestamps
	   ClassiCube prints in chat logs. */
	localtime_r(&s, &loc);

	t->year   = loc.tm_year + 1900;
	t->month  = loc.tm_mon  + 1;
	t->day    = loc.tm_mday;
	t->hour   = loc.tm_hour;
	t->minute = loc.tm_min;
	t->second = loc.tm_sec;
}


/*########################################################################################################################*
*--------------------------------------------------------Stopwatch--------------------------------------------------------*
*#########################################################################################################################*/
/* SYS_UPTIME_MS (252): monotonic milliseconds since boot, computed in-kernel as
   timer_ticks * 1000 / g_timer_hz. The kernel owns the tick rate, so userland
   never divides by a guessed frequency - which is exactly the bug that made
   OpenArena's frame timer run at 40% speed (#594).

   RESOLUTION IS 1 ms, and that is the honest ceiling available to Ring 3 today.
   RDTSC is readable from Ring 3 and would give sub-microsecond resolution, but
   only after calibrating its frequency against this same clock, and
   timer_ticks is known to arrive in BURSTS under vCPU starvation on KVM, which
   would corrupt the calibration. A wrong TSC frequency is a silent, permanent
   error in every frame delta; a coarse-but-correct millisecond clock is not.
   Revisit only if frame pacing measurably needs it. */
cc_uint64 Stopwatch_Measure(void) {
	return (cc_uint64)uptime_ms();
}

cc_uint64 Stopwatch_ElapsedMicroseconds(cc_uint64 beg, cc_uint64 end) {
	if (end < beg) return 0;
	return (end - beg) * 1000ULL;
}


/*########################################################################################################################*
*-----------------------------------------------------Directory/File------------------------------------------------------*
*#########################################################################################################################*/
/* EVERY path the engine opens comes through here, which is why the
   relative-path rewrite lives here and nowhere else.

   MayteraOS does not resolve relative paths against a per-process working
   directory. MEASURED on golden 1811: chdir(CC_MAYTERA_DATA_DIR) SUCCEEDS,
   mkdir("texpacks") then returns 0 and the kernel logs
   "[FS] Created directory: texpacks", and afterwards the directory exists
   nowhere on the image - the data directory is empty and there is no
   texpacks/ at either partition root. The first symptom the user sees is
   "Error 2 when saving ClassiCube textures / No such file or directory".

   ClassiCube is written entirely around relative paths, so each one is turned
   into an absolute path under the data directory here. Absolute paths are
   passed through untouched, so an explicit /FONTS or /GAMES path still works.
   This is the same hook Android's backend uses to prefix its app data dir. */
void Platform_EncodePath(cc_filepath* dst, const cc_string* path) {
	cc_string full; char fullBuffer[NATIVE_STR_LEN];

	if (path->length && path->buffer[0] == '/') {
		String_EncodeUtf8(dst->buffer, path);
		return;
	}

	/* String_AppendString/AppendConst are bounds-checked against the array
	   capacity and truncate rather than overrun, and String_EncodeUtf8 writes
	   at most NATIVE_STR_LEN bytes including the terminator, so a pathological
	   path is truncated into a failing open, never a buffer overflow. */
	String_InitArray(full, fullBuffer);
	String_AppendConst(&full, CC_MAYTERA_DATA_DIR);
	String_Append(&full, '/');
	String_AppendString(&full, path);
	String_EncodeUtf8(dst->buffer, &full);
}

void Platform_DecodePath(cc_string* dst, const cc_filepath* path) {
	const char* str = path->buffer;
	String_AppendUtf8(dst, str, String_Length(str));
}

void Directory_GetCachePath(cc_string* path) { }

cc_result Directory_Create2(const cc_filepath* path) {
	/* 0755. mkdir() in userland/libc/unistd.c DOES set errno (#578 fixed the
	   version that returned the raw syscall value and set nothing), so
	   EEXIST here is a real EEXIST and ReturnCode_DirectoryExists works. */
	return mkdir(path->buffer, 0755) == -1 ? (cc_result)errno : 0;
}

int File_Exists(const cc_filepath* path) {
	struct stat sb;
	return stat(path->buffer, &sb) == 0 && S_ISREG(sb.st_mode);
}

cc_result Directory_Enum(const cc_string* dirPath, void* obj, Directory_EnumCallback callback) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_filepath str;
	DIR* dirPtr;
	struct dirent* entry;
	const char* src;
	int len, is_dir;

	Platform_EncodePath(&str, dirPath);
	dirPtr = opendir(str.buffer);
	if (!dirPtr) return (cc_result)errno;

	String_InitArray(path, pathBuffer);

	while ((entry = readdir(dirPtr))) {
		path.length = 0;
		String_Format1(&path, "%s/", dirPath);

		/* ignore . and .. */
		src = entry->d_name;
		if (src[0] == '.' && src[1] == '\0') continue;
		if (src[0] == '.' && src[1] == '.' && src[2] == '\0') continue;

		len = String_Length(src);
		String_AppendUtf8(&path, src, len);

		is_dir = entry->d_type == DT_DIR;
		callback(&path, obj, is_dir);
	}

	/* Unlike POSIX readdir(), userland/libc/dirent.c's readdir() does not
	   distinguish end-of-directory from an error via errno - it returns NULL
	   for both - so there is no end-of-loop error to report. Reading errno
	   here would report whatever unrelated call last set it, which is the
	   exact stale-errno bug #578 documents. */
	closedir(dirPtr);
	return 0;
}

static cc_result File_Do(cc_file* file, const char* path, int mode) {
	*file = open(path, mode);
	return *file == -1 ? (cc_result)errno : 0;
}

cc_result File_Open(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDONLY);
}
cc_result File_Create(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDWR | O_CREAT | O_TRUNC);
}
cc_result File_OpenOrCreate(cc_file* file, const cc_filepath* path) {
	return File_Do(file, path->buffer, O_RDWR | O_CREAT);
}

cc_result File_Read(cc_file file, void* data, cc_uint32 count, cc_uint32* bytesRead) {
	/* Check the SIGNED return before narrowing. Platform_Posix.c assigns into
	   the cc_uint32 out-param first and then compares it against -1, which only
	   works because the truncation happens to produce 0xFFFFFFFF; do not copy
	   that. */
	long ret = read(file, data, count);
	if (ret < 0) { *bytesRead = 0; return (cc_result)errno; }
	*bytesRead = (cc_uint32)ret;
	return 0;
}

cc_result File_Write(cc_file file, const void* data, cc_uint32 count, cc_uint32* bytesWrote) {
	long ret = write(file, data, count);
	if (ret < 0) { *bytesWrote = 0; return (cc_result)errno; }
	*bytesWrote = (cc_uint32)ret;
	return 0;
}

cc_result File_Close(cc_file file) {
	/* NOTE for anyone chasing lost writes: on MayteraOS it is fsync(), not
	   close(), that proves bytes reached the medium (#695, see the contract on
	   fsync() in userland/libc/stdlib.h). ClassiCube's Stream contract has no
	   flush-to-disc step, so this port inherits "closed" == "handed to the
	   kernel". Do not "fix" that by adding a blind fsync in here - it would
	   fsync on every texture pack read too. */
	return close(file) == -1 ? (cc_result)errno : 0;
}

cc_result File_Seek(cc_file file, int offset, int seekType) {
	static const int modes[3] = { SEEK_SET, SEEK_CUR, SEEK_END };
	if (seekType < 0 || seekType > 2) return ERR_INVALID_ARGUMENT;
	return lseek(file, offset, modes[seekType]) == -1 ? (cc_result)errno : 0;
}

cc_result File_Position(cc_file file, cc_uint32* pos) {
	off_t cur = lseek(file, 0, SEEK_CUR);
	if (cur == -1) { *pos = 0; return (cc_result)errno; }
	*pos = (cc_uint32)cur;
	return 0;
}

cc_result File_Length(cc_file file, cc_uint32* len) {
	/* Deliberately lseek-based rather than fstat-based, unlike Platform_Posix.c.
	   fstat() exists here, but its st_size has to be right on BOTH filesystems
	   this OS mounts (ext2 root and the FAT ESP), while SEEK_END has to be right
	   anyway for every Stream this engine uses. One mechanism that is already
	   load-bearing beats a second one that is only exercised here. */
	off_t cur, end;

	cur = lseek(file, 0, SEEK_CUR);
	if (cur == -1) { *len = 0; return (cc_result)errno; }

	end = lseek(file, 0, SEEK_END);
	if (end == -1) { *len = 0; return (cc_result)errno; }

	/* Restore the position, and report a failure to restore it: silently
	   leaving the file offset at EOF would corrupt the caller's next read. */
	if (lseek(file, cur, SEEK_SET) == -1) { *len = 0; return (cc_result)errno; }

	*len = (cc_uint32)end;
	return 0;
}


/*########################################################################################################################*
*--------------------------------------------------------Threading--------------------------------------------------------*
*#########################################################################################################################*/
void Thread_Sleep(cc_uint32 milliseconds) {
	/* Not usleep(): userland/libc/unistd.c's usleep rounds UP and clamps to a
	   minimum of 1 ms, so usleep(0) sleeps. Thread_Sleep(0) means "give up the
	   rest of my slice", which is yield(). */
	if (!milliseconds) { yield(); return; }
	sys_sleep(milliseconds);
}

static void* ExecThread(void* param) {
	((Thread_StartFunc)param)();
	return NULL;
}

void Thread_Run(void** handle, Thread_StartFunc func, int stackSize, const char* name) {
	pthread_t* ptr = (pthread_t*)Mem_Alloc(1, sizeof(pthread_t), "thread");
	pthread_attr_t attrs;
	int res;

	*handle = ptr;
	pthread_attr_init(&attrs);
	if (stackSize > 0) pthread_attr_setstacksize(&attrs, (unsigned long)stackSize);

	res = pthread_create(ptr, &attrs, ExecThread, (void*)func);
	if (res) Process_Abort2((cc_result)res, "Creating thread");
	pthread_attr_destroy(&attrs);

	/* MayteraOS has no pthread_setname_np and no per-thread name in the kernel
	   process table (proc_info_t carries one name per process). The name is
	   logged instead so a hung thread can still be identified from serial. */
	Platform_Log1("thread started: %c", name);
}

void Thread_Detach(void* handle) {
	pthread_t* ptr = (pthread_t*)handle;

	/* KNOWN LEAK, and it is in the libc, not here: pthread_detach() in
	   userland/libc/pthread.c is `return 0;` with a comment saying the thread
	   "will clean up on exit", which it does not - the thread's malloc'd stack,
	   its thread_start_t and its join word are all freed only by
	   pthread_join(). So every detached thread leaks ~64 KB + 2 small blocks.
	   Reported as a libc gap; do NOT work around it here by joining, which
	   would block the caller. */
	(void)pthread_detach(*ptr);
	Mem_Free(ptr);
}

void Thread_Join(void* handle) {
	pthread_t* ptr = (pthread_t*)handle;
	int res = pthread_join(*ptr, NULL);
	if (res) Process_Abort2((cc_result)res, "Joining thread");
	Mem_Free(ptr);
}

void* Mutex_Create(const char* name) {
	pthread_mutex_t* ptr = (pthread_mutex_t*)Mem_Alloc(1, sizeof(pthread_mutex_t), "mutex");
	int res = pthread_mutex_init(ptr, NULL);
	if (res) Process_Abort2((cc_result)res, "Creating mutex");
	return ptr;
}

void Mutex_Free(void* handle) {
	int res = pthread_mutex_destroy((pthread_mutex_t*)handle);
	if (res) Process_Abort2((cc_result)res, "Destroying mutex");
	Mem_Free(handle);
}

void Mutex_Lock(void* handle) {
	int res = pthread_mutex_lock((pthread_mutex_t*)handle);
	if (res) Process_Abort2((cc_result)res, "Locking mutex");
}

void Mutex_Unlock(void* handle) {
	int res = pthread_mutex_unlock((pthread_mutex_t*)handle);
	if (res) Process_Abort2((cc_result)res, "Unlocking mutex");
}

/* ---------------------------------------------------------------------------
 * Waitable: an AUTO-RESET event, built straight on the futex layer.
 *
 * WHY NOT pthread_cond_timedwait, which is what Platform_Posix.c uses:
 * userland/libc/pthread.c's pthread_cond_timedwait treats its `abstime`
 * argument as a RELATIVE duration -
 *     timeout_ms = abstime->tv_sec * 1000 + abstime->tv_nsec / 1000000
 * - and its own comment admits "this is a simplification". POSIX abstime is an
 * ABSOLUTE time, so passing a correct abstime (~1.8e9 seconds since the epoch)
 * asks for a ~20-day timeout. Waitable_WaitFor would never time out. That is a
 * libc bug and is reported as one; this file must not depend on it either way.
 *
 * THIS IS NOT A HAND-ROLLED POLL LOOP (the project-wide banned pattern):
 * futex_wait() parks the caller on the kernel's futex wait queue
 * (kernel/sync/futex.c, which blocks through the real scheduler), and the loop
 * can only re-run after a genuine wake, a timeout, or -EAGAIN. -EAGAIN means
 * the flag is already set, so the very next exchange succeeds and the loop
 * exits. There is no path that spins.
 *
 * TRAP, and the reason WaitFor checks the deadline BEFORE calling futex_wait:
 * futex_wait()'s timeout argument is RELATIVE MILLISECONDS and **0 MEANS WAIT
 * FOREVER**, not "return immediately". Waitable_WaitFor(h, 0) must therefore
 * never reach futex_wait.
 * ------------------------------------------------------------------------- */
struct WaitData { volatile unsigned int state; }; /* 0 = clear, 1 = signalled */

void* Waitable_Create(const char* name) {
	struct WaitData* ptr = (struct WaitData*)Mem_Alloc(1, sizeof(struct WaitData), "waitable");
	ptr->state = 0;
	return ptr;
}

void Waitable_Free(void* handle) {
	Mem_Free(handle);
}

void Waitable_Signal(void* handle) {
	struct WaitData* ptr = (struct WaitData*)handle;

	__atomic_store_n(&ptr->state, 1u, __ATOMIC_SEQ_CST);
	/* Wake everyone: a Waitable may legitimately have several waiters, and the
	   auto-reset exchange below means only one of them consumes the signal. */
	futex_wake(&ptr->state, 0x7FFFFFFF);
}

void Waitable_Wait(void* handle) {
	struct WaitData* ptr = (struct WaitData*)handle;

	for (;;) {
		if (__atomic_exchange_n(&ptr->state, 0u, __ATOMIC_SEQ_CST)) return;
		futex_wait(&ptr->state, 0u, 0); /* 0 = no timeout */
	}
}

void Waitable_WaitFor(void* handle, cc_uint32 milliseconds) {
	struct WaitData* ptr = (struct WaitData*)handle;
	unsigned long deadline = uptime_ms() + milliseconds;

	for (;;) {
		unsigned long now;
		if (__atomic_exchange_n(&ptr->state, 0u, __ATOMIC_SEQ_CST)) return;

		now = uptime_ms();
		/* Signed difference so this stays correct if the clock wraps. */
		if ((long)(now - deadline) >= 0) return; /* timed out */

		futex_wait(&ptr->state, 0u, deadline - now);
	}
}


/*########################################################################################################################*
*--------------------------------------------------------Font/Text--------------------------------------------------------*
*#########################################################################################################################*/
static void FontDirCallback(const cc_string* path, void* obj, int isDirectory) {
	static const cc_string fonExt = String_FromConst(".fon");
	/* Completely skip windows .FON files */
	if (String_CaselessEnds(path, &fonExt)) return;

	SysFonts_Register(path, NULL);
}

void Platform_LoadSysFonts(void) {
	static const cc_string dir = String_FromConst(CC_MAYTERA_FONTS_DIR);

	/* NOTE: with CC_BUILD_FREETYPE undefined (the current default, see the
	   PLAT_MAYTERA branch in Core.h) SysFonts_Register is the stub that returns
	   ERR_NOT_SUPPORTED and nothing calls this function at all. It is written
	   anyway so that enabling freetype is a one-flag change rather than a
	   one-flag change plus a missing directory walk.

	   MayteraOS also has an OS-wide font registry of its own (SYS_FONT_COUNT /
	   SYS_FONT_GLYPH / SYS_FONT_METRICS, userland/libc/syscall.h). Rendering
	   text through THAT instead of freetype would be the better long-term
	   answer, and would need a SystemFonts backend rather than this walk. */
	Directory_Enum(&dir, NULL, FontDirCallback);
}


/*########################################################################################################################*
*-----------------------------------------------------Process/Module------------------------------------------------------*
*#########################################################################################################################*/
cc_result Process_StartGame2(const cc_string* args, int numArgs) {
	/* Single-process: hand the arguments to the in-process game loop rather
	   than spawning a second copy of ourselves. SetGameArgs is the shared
	   helper in _PlatformBase.h. */
	return SetGameArgs(args, numArgs);
}

void Process_Exit(cc_result code) {
	exit((int)code);
}


/*########################################################################################################################*
*--------------------------------------------------------Platform---------------------------------------------------------*
*#########################################################################################################################*/
void Platform_Init(void) {
	cc_uintptr addr;

	/* Log the runtime address of a known function. MayteraOS user apps are PIE
	   (userland/user-pie.ld) and the kernel picks the load base, so this is the
	   value you need to turn a faulting RIP from a serial panic back into a
	   source line with addr2line. */
	addr = (cc_uintptr)Process_Exit;
	Platform_Log1("Process_Exit addr: %x", &addr);
}

void Platform_Free(void) { }

cc_bool Platform_DescribeError(cc_result res, cc_string* dst) {
	const char* err;

	/* ClassiCube's own error codes start at 0xCCDED000 and are described by
	   Logger.c, not here. Anything under 1000 is an errno from userland/libc. */
	if (res >= 1000) return false;

	err = strerror((int)res);
	if (!err) return false;

	String_AppendUtf8(dst, err, String_Length(err));
	return true;
}

cc_result Platform_SetDefaultCurrentDirectory(void) {
	static const char* dir = CC_MAYTERA_DATA_DIR;
	struct stat st;

	/* Create the data directory if this is a first run. Anything other than
	   EEXIST is reported, because a game that cannot write its data directory
	   should say so at startup rather than fail one texture pack at a time
	   later.

	   EEXIST IS NOT AUTOMATICALLY SUCCESS. It only means "something is already
	   at that path", and if that something is a FILE, the chdir below fails and
	   the game silently runs in whatever directory it inherited, writing
	   texpacks/, audio/, maps/ and options.txt there. That is not hypothetical:
	   it is what happened when this constant pointed at /APPS/CLASSICUBE, the
	   path of this program's own executable. So the existing path is required
	   to be a directory. */
	if (mkdir(dir, 0755) == -1) {
		if (errno != EEXIST) return (cc_result)errno;
		if (stat(dir, &st) == -1)  return (cc_result)errno;
		if (!S_ISDIR(st.st_mode))  return (cc_result)ENOTDIR;
	}

	if (chdir(dir) == -1) return (cc_result)errno;
	return 0;
}


/*########################################################################################################################*
*-----------------------------------------------------Main entrypoint-----------------------------------------------------*
*#########################################################################################################################*/
#include "main_impl.h"

int main(int argc, char** argv) {
	cc_result res;
	SetupProgram(argc, argv);

	/* Single process, so the loop is launcher -> game -> launcher etc. */
	do {
		res = RunProgram(argc, argv);
	} while (Platform_IsSingleProcess() && Window_Main.Exists);

	Window_Free();
	Process_Exit(res);
	return (int)res;
}

#endif /* CC_BUILD_MAYTERA */
