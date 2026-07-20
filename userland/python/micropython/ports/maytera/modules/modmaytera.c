// modmaytera.c - MayteraOS-specific Python module
// Provides access to MayteraOS GUI and system functions

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"

// Syscall definitions
#define SYS_WIN_CREATE      30
#define SYS_WIN_DESTROY     31
#define SYS_WIN_DRAW_RECT   32
#define SYS_WIN_DRAW_TEXT   33
#define SYS_WIN_DRAW_PIXEL  34
#define SYS_WIN_BLIT        35
#define SYS_WIN_GET_EVENT   36
#define SYS_WIN_INVALIDATE  37
#define SYS_GETPID          4
#define SYS_SLEEP           7
#define SYS_TIME            50
#define SYS_CLOCK           51

// Syscall wrappers
static inline long syscall0(long num) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(num) : "rcx", "r11", "memory");
    return result;
}

static inline long syscall1(long num, long a1) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(num), "D"(a1) : "rcx", "r11", "memory");
    return result;
}

static inline long syscall2(long num, long a1, long a2) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(num), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return result;
}

static inline long syscall5(long num, long a1, long a2, long a3, long a4, long a5) {
    long result;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    asm volatile("syscall" : "=a"(result) 
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8) 
                 : "rcx", "r11", "memory");
    return result;
}

static inline long syscall6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long result;
    register long r10 asm("r10") = a4;
    register long r8 asm("r8") = a5;
    register long r9 asm("r9") = a6;
    asm volatile("syscall" : "=a"(result) 
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) 
                 : "rcx", "r11", "memory");
    return result;
}

// ============================================================================
// Window Class
// ============================================================================

typedef struct _maytera_window_obj_t {
    mp_obj_base_t base;
    int handle;
    int width;
    int height;
} maytera_window_obj_t;

const mp_obj_type_t maytera_window_type;

// Window constructor
STATIC mp_obj_t maytera_window_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 4, 5, false);
    
    const char *title = mp_obj_str_get_str(args[0]);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    int width = mp_obj_get_int(args[3]);
    int height = n_args > 4 ? mp_obj_get_int(args[4]) : 200;
    
    // Create window via syscall
    int handle = (int)syscall5(SYS_WIN_CREATE, (long)title, x, y, width, height);
    
    if (handle < 0) {
        mp_raise_OSError(-handle);
    }
    
    maytera_window_obj_t *self = mp_obj_malloc(maytera_window_obj_t, type);
    self->handle = handle;
    self->width = width;
    self->height = height;
    
    return MP_OBJ_FROM_PTR(self);
}

// Window.draw_rect(x, y, width, height, color)
STATIC mp_obj_t maytera_window_draw_rect(size_t n_args, const mp_obj_t *args) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    int w = mp_obj_get_int(args[3]);
    int h = mp_obj_get_int(args[4]);
    uint32_t color = mp_obj_get_int(args[5]);
    
    syscall6(SYS_WIN_DRAW_RECT, self->handle, x, y, w, h, color);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(maytera_window_draw_rect_obj, 6, 6, maytera_window_draw_rect);

// Window.draw_text(x, y, text, color)
STATIC mp_obj_t maytera_window_draw_text(size_t n_args, const mp_obj_t *args) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    const char *text = mp_obj_str_get_str(args[3]);
    uint32_t color = mp_obj_get_int(args[4]);
    
    syscall5(SYS_WIN_DRAW_TEXT, self->handle, x, y, (long)text, color);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(maytera_window_draw_text_obj, 5, 5, maytera_window_draw_text);

// Window.draw_pixel(x, y, color)
STATIC mp_obj_t maytera_window_draw_pixel(size_t n_args, const mp_obj_t *args) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int x = mp_obj_get_int(args[1]);
    int y = mp_obj_get_int(args[2]);
    uint32_t color = mp_obj_get_int(args[3]);
    
    // Encode x, y, color into syscall
    long xy = ((long)x << 32) | (y & 0xFFFFFFFF);
    syscall2(SYS_WIN_DRAW_PIXEL, self->handle, (xy << 32) | color);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(maytera_window_draw_pixel_obj, 4, 4, maytera_window_draw_pixel);

// Window.invalidate()
STATIC mp_obj_t maytera_window_invalidate(mp_obj_t self_in) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(self_in);
    syscall1(SYS_WIN_INVALIDATE, self->handle);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(maytera_window_invalidate_obj, maytera_window_invalidate);

// Window.close()
STATIC mp_obj_t maytera_window_close(mp_obj_t self_in) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->handle >= 0) {
        syscall1(SYS_WIN_DESTROY, self->handle);
        self->handle = -1;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(maytera_window_close_obj, maytera_window_close);

// Window.get_event(timeout_ms) -> dict or None
STATIC mp_obj_t maytera_window_get_event(size_t n_args, const mp_obj_t *args) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int timeout = n_args > 1 ? mp_obj_get_int(args[1]) : 0;
    
    // Event buffer
    struct {
        int type;
        int x, y;
        int button;
        int keycode;
        char key_char;
    } event;
    
    // We need a 3-arg syscall for this
    long result;
    register long r10 asm("r10") = timeout;
    asm volatile("syscall" : "=a"(result) 
                 : "a"((long)SYS_WIN_GET_EVENT), "D"((long)self->handle), "S"((long)&event), "d"(sizeof(event)), "r"(r10)
                 : "rcx", "r11", "memory");
    
    if (result == 0) {
        return mp_const_none;  // No event
    }
    
    // Create dict with event info
    mp_obj_t dict = mp_obj_new_dict(5);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_type), mp_obj_new_int(event.type));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_x), mp_obj_new_int(event.x));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_y), mp_obj_new_int(event.y));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_button), mp_obj_new_int(event.button));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_keycode), mp_obj_new_int(event.keycode));
    
    return dict;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(maytera_window_get_event_obj, 1, 2, maytera_window_get_event);

// Window properties
STATIC void maytera_window_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    maytera_window_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // Load attribute
        if (attr == MP_QSTR_handle) {
            dest[0] = mp_obj_new_int(self->handle);
        } else if (attr == MP_QSTR_width) {
            dest[0] = mp_obj_new_int(self->width);
        } else if (attr == MP_QSTR_height) {
            dest[0] = mp_obj_new_int(self->height);
        }
    }
}

// Window methods table
STATIC const mp_rom_map_elem_t maytera_window_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_draw_rect), MP_ROM_PTR(&maytera_window_draw_rect_obj) },
    { MP_ROM_QSTR(MP_QSTR_draw_text), MP_ROM_PTR(&maytera_window_draw_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_draw_pixel), MP_ROM_PTR(&maytera_window_draw_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_invalidate), MP_ROM_PTR(&maytera_window_invalidate_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&maytera_window_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_event), MP_ROM_PTR(&maytera_window_get_event_obj) },
};
STATIC MP_DEFINE_CONST_DICT(maytera_window_locals_dict, maytera_window_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    maytera_window_type,
    MP_QSTR_Window,
    MP_TYPE_FLAG_NONE,
    make_new, maytera_window_make_new,
    attr, maytera_window_attr,
    locals_dict, &maytera_window_locals_dict
);

// ============================================================================
// Module-level Functions
// ============================================================================

// maytera.getpid() -> int
STATIC mp_obj_t maytera_getpid(void) {
    return mp_obj_new_int(syscall0(SYS_GETPID));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(maytera_getpid_obj, maytera_getpid);

// maytera.sleep(ms)
STATIC mp_obj_t maytera_sleep(mp_obj_t ms_in) {
    int ms = mp_obj_get_int(ms_in);
    syscall1(SYS_SLEEP, ms);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(maytera_sleep_obj, maytera_sleep);

// maytera.time() -> int (system time)
STATIC mp_obj_t maytera_time(void) {
    return mp_obj_new_int(syscall0(SYS_TIME));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(maytera_time_obj, maytera_time);

// maytera.ticks() -> int (system ticks)
STATIC mp_obj_t maytera_ticks(void) {
    return mp_obj_new_int(syscall0(SYS_CLOCK));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(maytera_ticks_obj, maytera_ticks);

// maytera.version() -> str
STATIC mp_obj_t maytera_version(void) {
    return mp_obj_new_str("MayteraOS 1.0", 13);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(maytera_version_obj, maytera_version);

// Event type constants
#define EVENT_NONE          0
#define EVENT_REDRAW        1
#define EVENT_KEY_DOWN      2
#define EVENT_KEY_UP        3
#define EVENT_MOUSE_DOWN    4
#define EVENT_MOUSE_UP      5
#define EVENT_MOUSE_MOVE    6
#define EVENT_WINDOW_CLOSE  7

// ============================================================================
// Module Definition
// ============================================================================

STATIC const mp_rom_map_elem_t mp_module_maytera_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_maytera) },
    
    // Classes
    { MP_ROM_QSTR(MP_QSTR_Window), MP_ROM_PTR(&maytera_window_type) },
    
    // Functions
    { MP_ROM_QSTR(MP_QSTR_getpid), MP_ROM_PTR(&maytera_getpid_obj) },
    { MP_ROM_QSTR(MP_QSTR_sleep), MP_ROM_PTR(&maytera_sleep_obj) },
    { MP_ROM_QSTR(MP_QSTR_time), MP_ROM_PTR(&maytera_time_obj) },
    { MP_ROM_QSTR(MP_QSTR_ticks), MP_ROM_PTR(&maytera_ticks_obj) },
    { MP_ROM_QSTR(MP_QSTR_version), MP_ROM_PTR(&maytera_version_obj) },
    
    // Event type constants
    { MP_ROM_QSTR(MP_QSTR_EVENT_NONE), MP_ROM_INT(EVENT_NONE) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_REDRAW), MP_ROM_INT(EVENT_REDRAW) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_KEY_DOWN), MP_ROM_INT(EVENT_KEY_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_KEY_UP), MP_ROM_INT(EVENT_KEY_UP) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_MOUSE_DOWN), MP_ROM_INT(EVENT_MOUSE_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_MOUSE_UP), MP_ROM_INT(EVENT_MOUSE_UP) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_MOUSE_MOVE), MP_ROM_INT(EVENT_MOUSE_MOVE) },
    { MP_ROM_QSTR(MP_QSTR_EVENT_WINDOW_CLOSE), MP_ROM_INT(EVENT_WINDOW_CLOSE) },
    
    // Color helpers
    { MP_ROM_QSTR(MP_QSTR_RGB), MP_ROM_INT(0) },  // Placeholder, could be function
};
STATIC MP_DEFINE_CONST_DICT(mp_module_maytera_globals, mp_module_maytera_globals_table);

const mp_obj_module_t mp_module_maytera = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_maytera_globals,
};

MP_REGISTER_MODULE(MP_QSTR_maytera, mp_module_maytera);
