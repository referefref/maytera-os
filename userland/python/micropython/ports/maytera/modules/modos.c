// modos.c - OS module for MayteraOS MicroPython
// Provides file and directory operations

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/stream.h"

// Syscall definitions
#define SYS_OPEN        10
#define SYS_CLOSE       11
#define SYS_READ        12
#define SYS_WRITE       13
#define SYS_SEEK        14
#define SYS_STAT        15
#define SYS_MKDIR       16
#define SYS_RMDIR       17
#define SYS_UNLINK      18
#define SYS_READDIR     19
#define SYS_GETPID      4
#define SYS_FORK        1
#define SYS_EXIT        0
#define SYS_EXEC        2
#define SYS_WAIT        3

// File open flags
#define O_RDONLY    0
#define O_WRONLY    1
#define O_RDWR      2
#define O_CREAT     0x100
#define O_TRUNC     0x200
#define O_APPEND    0x400

// Seek modes
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

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

static inline long syscall3(long num, long a1, long a2, long a3) {
    long result;
    asm volatile("syscall" : "=a"(result) : "a"(num), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return result;
}

// ============================================================================
// File Operations
// ============================================================================

// os.open(path, flags) -> fd
STATIC mp_obj_t mod_os_open(size_t n_args, const mp_obj_t *args) {
    const char *path = mp_obj_str_get_str(args[0]);
    int flags = n_args > 1 ? mp_obj_get_int(args[1]) : O_RDONLY;
    
    int fd = (int)syscall2(SYS_OPEN, (long)path, flags);
    if (fd < 0) {
        mp_raise_OSError(-fd);
    }
    return mp_obj_new_int(fd);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_os_open_obj, 1, 2, mod_os_open);

// os.close(fd)
STATIC mp_obj_t mod_os_close(mp_obj_t fd_in) {
    int fd = mp_obj_get_int(fd_in);
    int result = (int)syscall1(SYS_CLOSE, fd);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_close_obj, mod_os_close);

// os.read(fd, size) -> bytes
STATIC mp_obj_t mod_os_read(mp_obj_t fd_in, mp_obj_t size_in) {
    int fd = mp_obj_get_int(fd_in);
    size_t size = mp_obj_get_int(size_in);
    
    // Allocate buffer
    vstr_t vstr;
    vstr_init_len(&vstr, size);
    
    long n = syscall3(SYS_READ, fd, (long)vstr.buf, size);
    if (n < 0) {
        vstr_clear(&vstr);
        mp_raise_OSError(-n);
    }
    
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_os_read_obj, mod_os_read);

// os.write(fd, data) -> int
STATIC mp_obj_t mod_os_write(mp_obj_t fd_in, mp_obj_t data_in) {
    int fd = mp_obj_get_int(fd_in);
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    
    long n = syscall3(SYS_WRITE, fd, (long)bufinfo.buf, bufinfo.len);
    if (n < 0) {
        mp_raise_OSError(-n);
    }
    return mp_obj_new_int(n);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_os_write_obj, mod_os_write);

// os.lseek(fd, offset, whence) -> int
STATIC mp_obj_t mod_os_lseek(mp_obj_t fd_in, mp_obj_t offset_in, mp_obj_t whence_in) {
    int fd = mp_obj_get_int(fd_in);
    long offset = mp_obj_get_int(offset_in);
    int whence = mp_obj_get_int(whence_in);
    
    long result = syscall3(SYS_SEEK, fd, offset, whence);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_obj_new_int(result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_3(mod_os_lseek_obj, mod_os_lseek);

// ============================================================================
// Directory Operations
// ============================================================================

// os.mkdir(path)
STATIC mp_obj_t mod_os_mkdir(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    int result = (int)syscall1(SYS_MKDIR, (long)path);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_mkdir_obj, mod_os_mkdir);

// os.rmdir(path)
STATIC mp_obj_t mod_os_rmdir(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    int result = (int)syscall1(SYS_RMDIR, (long)path);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_rmdir_obj, mod_os_rmdir);

// os.remove(path) / os.unlink(path)
STATIC mp_obj_t mod_os_remove(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    int result = (int)syscall1(SYS_UNLINK, (long)path);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_remove_obj, mod_os_remove);

// os.listdir(path) -> list
STATIC mp_obj_t mod_os_listdir(size_t n_args, const mp_obj_t *args) {
    const char *path = n_args > 0 ? mp_obj_str_get_str(args[0]) : ".";
    
    int fd = (int)syscall2(SYS_OPEN, (long)path, O_RDONLY);
    if (fd < 0) {
        mp_raise_OSError(-fd);
    }
    
    mp_obj_t list = mp_obj_new_list(0, NULL);
    
    // Read directory entries
    // Assuming kernel returns null-terminated names
    char buf[256];
    long n;
    while ((n = syscall3(SYS_READDIR, fd, (long)buf, sizeof(buf))) > 0) {
        // Each entry is a null-terminated string
        const char *p = buf;
        while (p < buf + n && *p) {
            mp_obj_t name = mp_obj_new_str(p, strlen(p));
            mp_obj_list_append(list, name);
            p += strlen(p) + 1;
        }
    }
    
    syscall1(SYS_CLOSE, fd);
    return list;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_os_listdir_obj, 0, 1, mod_os_listdir);

// ============================================================================
// Process Operations
// ============================================================================

// os.getpid() -> int
STATIC mp_obj_t mod_os_getpid(void) {
    return mp_obj_new_int(syscall0(SYS_GETPID));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_os_getpid_obj, mod_os_getpid);

// os._exit(code)
STATIC mp_obj_t mod_os_exit(mp_obj_t code_in) {
    int code = mp_obj_get_int(code_in);
    syscall1(SYS_EXIT, code);
    // Should not return
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_exit_obj, mod_os_exit);

// os.getcwd() -> str
static char current_dir[256] = "/";

STATIC mp_obj_t mod_os_getcwd(void) {
    return mp_obj_new_str(current_dir, strlen(current_dir));
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_os_getcwd_obj, mod_os_getcwd);

// os.chdir(path)
STATIC mp_obj_t mod_os_chdir(mp_obj_t path_in) {
    const char *path = mp_obj_str_get_str(path_in);
    size_t len = strlen(path);
    if (len >= sizeof(current_dir)) {
        mp_raise_OSError(22);  // EINVAL
    }
    
    // Verify path exists by trying to open it
    int fd = (int)syscall2(SYS_OPEN, (long)path, O_RDONLY);
    if (fd < 0) {
        mp_raise_OSError(-fd);
    }
    syscall1(SYS_CLOSE, fd);
    
    // Copy path
    memcpy(current_dir, path, len + 1);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(mod_os_chdir_obj, mod_os_chdir);

// os.uname() -> tuple
STATIC mp_obj_t mod_os_uname(void) {
    mp_obj_t items[5] = {
        mp_obj_new_str("MayteraOS", 9),        // sysname
        mp_obj_new_str("maytera", 7),          // nodename
        mp_obj_new_str("1.0", 3),              // release
        mp_obj_new_str("1.0.0", 5),            // version
        mp_obj_new_str("x86_64", 6),           // machine
    };
    return mp_obj_new_tuple(5, items);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_0(mod_os_uname_obj, mod_os_uname);

// String length helper
static size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}

// Memory copy helper
static void *memcpy(void *dst, const void *src, size_t n) {
    char *d = dst;
    const char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

// ============================================================================
// Module Definition
// ============================================================================

STATIC const mp_rom_map_elem_t mp_module_os_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_os) },
    
    // File operations
    { MP_ROM_QSTR(MP_QSTR_open), MP_ROM_PTR(&mod_os_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&mod_os_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_read), MP_ROM_PTR(&mod_os_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&mod_os_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_lseek), MP_ROM_PTR(&mod_os_lseek_obj) },
    
    // Directory operations
    { MP_ROM_QSTR(MP_QSTR_mkdir), MP_ROM_PTR(&mod_os_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir), MP_ROM_PTR(&mod_os_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove), MP_ROM_PTR(&mod_os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_unlink), MP_ROM_PTR(&mod_os_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_listdir), MP_ROM_PTR(&mod_os_listdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd), MP_ROM_PTR(&mod_os_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir), MP_ROM_PTR(&mod_os_chdir_obj) },
    
    // Process operations
    { MP_ROM_QSTR(MP_QSTR_getpid), MP_ROM_PTR(&mod_os_getpid_obj) },
    { MP_ROM_QSTR(MP_QSTR__exit), MP_ROM_PTR(&mod_os_exit_obj) },
    { MP_ROM_QSTR(MP_QSTR_uname), MP_ROM_PTR(&mod_os_uname_obj) },
    
    // File open flags
    { MP_ROM_QSTR(MP_QSTR_O_RDONLY), MP_ROM_INT(O_RDONLY) },
    { MP_ROM_QSTR(MP_QSTR_O_WRONLY), MP_ROM_INT(O_WRONLY) },
    { MP_ROM_QSTR(MP_QSTR_O_RDWR), MP_ROM_INT(O_RDWR) },
    { MP_ROM_QSTR(MP_QSTR_O_CREAT), MP_ROM_INT(O_CREAT) },
    { MP_ROM_QSTR(MP_QSTR_O_TRUNC), MP_ROM_INT(O_TRUNC) },
    { MP_ROM_QSTR(MP_QSTR_O_APPEND), MP_ROM_INT(O_APPEND) },
    
    // Seek modes
    { MP_ROM_QSTR(MP_QSTR_SEEK_SET), MP_ROM_INT(SEEK_SET) },
    { MP_ROM_QSTR(MP_QSTR_SEEK_CUR), MP_ROM_INT(SEEK_CUR) },
    { MP_ROM_QSTR(MP_QSTR_SEEK_END), MP_ROM_INT(SEEK_END) },
    
    // Path separator
    { MP_ROM_QSTR(MP_QSTR_sep), MP_ROM_QSTR(MP_QSTR__slash_) },
};
STATIC MP_DEFINE_CONST_DICT(mp_module_os_globals, mp_module_os_globals_table);

const mp_obj_module_t mp_module_os = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_os_globals,
};

MP_REGISTER_MODULE(MP_QSTR_os, mp_module_os);
