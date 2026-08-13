/* gfxtest_support.c - offscreen window backing + the minimum ClassiCube
 * platform surface the graphics backend touches (#28, gfx lane).
 *
 * PORTABLE. Compiled unchanged into both the host harness and the MayteraOS
 * ELF. It deliberately depends on nothing but malloc/free/memcpy/memset, which
 * both the host libc and the MayteraOS freestanding libc provide, so it does
 * not drag either build into the other's headers.
 *
 * The Window_*Framebuffer trio here is the OFFSCREEN stand-in for the real
 * compositor-backed implementation that the window lane owns. It implements
 * exactly the contract written down in gfx/mos_gfx.h section 2, including the
 * tightly-packed-stride requirement, which is the point: if the real one obeys
 * the same contract, it drops straight in.
 */
#include "Core.h"
#include "Bitmap.h"
#include "Platform.h"
#include "String_.h"
#include "Event.h"
#include "Game.h"
#include "Options.h"
#include "Graphics.h"
#include "gfxtest.h"

/* Declared here rather than via <stdlib.h>/<string.h> so this file compiles
 * identically under -nostdinc (MayteraOS) and hosted (build server). */
extern void* malloc(unsigned long size);
extern void  free(void* ptr);
extern void* memcpy(void* dst, const void* src, unsigned long n);
extern void* memset(void* dst, int c, unsigned long n);

/* ---------------------------------------------------------------------- */
/* Offscreen window: the framebuffer WE own                                */
/* ---------------------------------------------------------------------- */
static BitmapCol* fb_pixels;
static int fb_w, fb_h;
static int present_count;

BitmapCol* gt_framebuffer(void)  { return fb_pixels; }
void       gt_offscreen_reset(void) { present_count = 0; }
int        gt_present_count(void)   { return present_count; }

void Window_AllocFramebuffer(struct Bitmap* bmp, int width, int height) {
	/* Heap, never a static array: user.ld links a single RWX PT_LOAD and a
	 * multi-megabyte .bss breaks the MayteraOS ELF loader. */
	unsigned long bytes = (unsigned long)width * (unsigned long)height * 4UL;

	if (fb_pixels) { free(fb_pixels); fb_pixels = NULL; }
	fb_pixels = (BitmapCol*)malloc(bytes);

	if (!fb_pixels) {
		/* Fail loudly. A NULL scan0 handed back to the rasteriser is a
		 * guaranteed wild write, and it would look like a random crash rather
		 * than an out-of-memory condition. */
		Process_Abort("Window_AllocFramebuffer: out of memory");
		return;
	}
	memset(fb_pixels, 0, bytes);

	fb_w = width; fb_h = height;
	/* Stride IS width: Graphics_SoftGPU.c:1052-1053 assigns
	 * cb_stride = fb_bmp.width, so any row padding would skew the image. */
	bmp->scan0  = fb_pixels;
	bmp->width  = width;
	bmp->height = height;
}

void Window_DrawFramebuffer(Rect2D r, struct Bitmap* bmp) {
	/* Offscreen: nothing to present to, so just record that the backend asked.
	 * The real implementation copies bmp->scan0 into the compositor back
	 * buffer (identical pixel format, so a memcpy) and presents. */
	(void)r; (void)bmp;
	present_count++;
}

void Window_FreeFramebuffer(struct Bitmap* bmp) {
	if (fb_pixels) { free(fb_pixels); fb_pixels = NULL; }
	if (bmp) bmp->scan0 = NULL;
	fb_w = fb_h = 0;
}

/* ---------------------------------------------------------------------- */
/* Memory                                                                  */
/* ---------------------------------------------------------------------- */
void* Mem_TryAlloc(cc_uint32 numElems, cc_uint32 elemsSize) {
	cc_uint64 bytes = (cc_uint64)numElems * elemsSize;
	if (!bytes || bytes > 0x7FFFFFFFULL) return NULL;   /* overflow guard */
	return malloc((unsigned long)bytes);
}

void* Mem_TryAllocCleared(cc_uint32 numElems, cc_uint32 elemsSize) {
	void* p = Mem_TryAlloc(numElems, elemsSize);
	if (p) memset(p, 0, (unsigned long)numElems * elemsSize);
	return p;
}

void* Mem_Alloc(cc_uint32 numElems, cc_uint32 elemsSize, const char* place) {
	void* p = Mem_TryAlloc(numElems, elemsSize);
	if (!p) Process_Abort2(0, place);
	return p;
}

void* Mem_AllocCleared(cc_uint32 numElems, cc_uint32 elemsSize, const char* place) {
	void* p = Mem_TryAllocCleared(numElems, elemsSize);
	if (!p) Process_Abort2(0, place);
	return p;
}

void  Mem_Free(void* mem) { if (mem) free(mem); }
void* Mem_Set(void* dst, cc_uint8 value, unsigned numBytes) { return memset(dst, value, numBytes); }
void* Mem_Copy(void* dst, const void* src, unsigned numBytes) { return memcpy(dst, src, numBytes); }

/* ---------------------------------------------------------------------- */
/* Events: real enough that Gfx_Component.Init() works                     */
/* ---------------------------------------------------------------------- */
struct _GfxEventsList GfxEvents;

void Event_Register(struct Event_Void* handlers, void* obj, Event_Void_Callback handler) {
	int i = handlers->Count;
	if (i >= EVENT_MAX_CALLBACKS) return;
	handlers->Handlers[i] = handler;
	handlers->Objs[i]     = obj;
	handlers->Count       = i + 1;
}

void Event_Unregister(struct Event_Void* handlers, void* obj, Event_Void_Callback handler) {
	int i, j;
	for (i = 0; i < handlers->Count; i++) {
		if (handlers->Handlers[i] != handler || handlers->Objs[i] != obj) continue;
		for (j = i; j < handlers->Count - 1; j++) {
			handlers->Handlers[j] = handlers->Handlers[j + 1];
			handlers->Objs[j]     = handlers->Objs[j + 1];
		}
		handlers->Count--;
		return;
	}
}

void Event_RaiseVoid(struct Event_Void* handlers) {
	int i;
	for (i = 0; i < handlers->Count; i++) handlers->Handlers[i](handlers->Objs[i]);
}

/* ---------------------------------------------------------------------- */
/* Remaining leaf dependencies                                             */
/* ---------------------------------------------------------------------- */
cc_bool Game_ReduceVRAM(void) { return false; }
cc_bool Options_GetBool(const char* key, cc_bool defValue) { (void)key; return defValue; }

void Platform_LogConst(const char* message) { gt_log(message); }
void Platform_Log1(const char* format, const void* a1) { (void)a1; gt_log(format); }

void String_Format1(cc_string* str, const char* format, const void* a1) {
	(void)str; (void)format; (void)a1;
}
void String_Format2(cc_string* str, const char* format, const void* a1, const void* a2) {
	(void)str; (void)format; (void)a1; (void)a2;
}
void String_Format3(cc_string* str, const char* format, const void* a1, const void* a2, const void* a3) {
	(void)str; (void)format; (void)a1; (void)a2; (void)a3;
}

/* Only reached from Gfx_TakeScreenshot, which this harness does not call:
 * the harness dumps a BMP itself so the evidence path has no PNG encoder in
 * it. Returning an error rather than 0 keeps a silent no-op from ever passing
 * for a successful screenshot. */
cc_result Png_Encode(struct Bitmap* bmp, struct Stream* stream,
                     Png_RowGetter getRow, cc_bool alpha, void* ctx) {
	(void)bmp; (void)stream; (void)getRow; (void)alpha; (void)ctx;
	return 1;
}
