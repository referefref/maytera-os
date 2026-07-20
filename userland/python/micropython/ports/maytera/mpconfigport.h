// mpconfigport.h - MicroPython configuration for MayteraOS
// This configures MicroPython for the MayteraOS environment

#ifndef MPCONFIGPORT_H
#define MPCONFIGPORT_H

// Python language features
#define MICROPY_PY_BUILTINS_SET         (1)
#define MICROPY_PY_BUILTINS_FROZENSET   (1)
#define MICROPY_PY_BUILTINS_PROPERTY    (1)
#define MICROPY_PY_BUILTINS_ENUMERATE   (1)
#define MICROPY_PY_BUILTINS_FILTER      (1)
#define MICROPY_PY_BUILTINS_REVERSED    (1)
#define MICROPY_PY_BUILTINS_SLICE       (1)
#define MICROPY_PY_BUILTINS_STR_OP_MODULO (1)
#define MICROPY_PY_BUILTINS_HELP        (1)
#define MICROPY_PY_BUILTINS_HELP_TEXT   maytera_help_text
#define MICROPY_PY_BUILTINS_HELP_MODULES (1)
#define MICROPY_PY_BUILTINS_INPUT       (1)
#define MICROPY_PY_BUILTINS_POW3        (1)
#define MICROPY_PY_BUILTINS_ROUND_INT   (1)
#define MICROPY_PY_BUILTINS_COMPLEX     (1)
#define MICROPY_PY_BUILTINS_BYTES_HEX   (1)
#define MICROPY_PY_BUILTINS_MEMORYVIEW  (1)

// Parser features
#define MICROPY_ENABLE_SOURCE_LINE      (1)
#define MICROPY_ERROR_REPORTING         (MICROPY_ERROR_REPORTING_DETAILED)
#define MICROPY_FLOAT_IMPL              (MICROPY_FLOAT_IMPL_DOUBLE)
#define MICROPY_LONGINT_IMPL            (MICROPY_LONGINT_IMPL_LONGLONG)

// Compiler features
#define MICROPY_COMP_CONST              (1)
#define MICROPY_COMP_DOUBLE_TUPLE_ASSIGN (1)
#define MICROPY_COMP_TRIPLE_TUPLE_ASSIGN (1)
#define MICROPY_COMP_RETURN_IF_EXPR     (1)

// VM features
#define MICROPY_ENABLE_GC               (1)
#define MICROPY_GC_ALLOC_THRESHOLD      (0)
#define MICROPY_ALLOC_PATH_MAX          (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT  (16)
#define MICROPY_ENABLE_FINALISER        (1)
#define MICROPY_ENABLE_SCHEDULER        (1)
#define MICROPY_SCHEDULER_DEPTH         (8)
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE (256)

// Stack size
#define MICROPY_STACK_CHECK             (1)
#define MICROPY_STACKLESS               (0)
#define MICROPY_STACKLESS_STRICT        (0)

// Module features
#define MICROPY_PY_ARRAY                (1)
#define MICROPY_PY_ARRAY_SLICE_ASSIGN   (1)
#define MICROPY_PY_ATTRTUPLE            (1)
#define MICROPY_PY_COLLECTIONS          (1)
#define MICROPY_PY_COLLECTIONS_DEQUE    (1)
#define MICROPY_PY_COLLECTIONS_ORDEREDDICT (1)
#define MICROPY_PY_MATH                 (1)
#define MICROPY_PY_CMATH                (1)
#define MICROPY_PY_IO                   (1)
#define MICROPY_PY_IO_IOBASE            (1)
#define MICROPY_PY_IO_FILEIO            (1)
#define MICROPY_PY_IO_BYTESIO           (1)
#define MICROPY_PY_IO_BUFFEREDWRITER    (1)
#define MICROPY_PY_STRUCT               (1)
#define MICROPY_PY_SYS                  (1)
#define MICROPY_PY_SYS_EXIT             (1)
#define MICROPY_PY_SYS_PLATFORM         "maytera"
#define MICROPY_PY_SYS_MAXSIZE          (1)
#define MICROPY_PY_SYS_PATH_DEFAULT     ".frozen:/lib/python"
#define MICROPY_PY_SYS_STDFILES         (1)
#define MICROPY_PY_SYS_STDIO_BUFFER     (1)
#define MICROPY_PY_GC                   (1)
#define MICROPY_PY_GC_COLLECT_RETVAL    (0)
#define MICROPY_PY_ERRNO                (1)
#define MICROPY_PY_THREAD               (0)  // Single-threaded for now
#define MICROPY_PY_THREAD_GIL           (0)
#define MICROPY_PY_RANDOM               (1)
#define MICROPY_PY_RANDOM_SEED_INIT_FUNC (mp_hal_time_ticks())
#define MICROPY_PY_TIME                 (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS    (1)
#define MICROPY_PY_TIME_INCLUDEFILE     "ports/maytera/modtime.c"
#define MICROPY_PY_JSON                 (1)
#define MICROPY_PY_RE                   (1)
#define MICROPY_PY_HEAPQ                (1)
#define MICROPY_PY_HASHLIB              (1)
#define MICROPY_PY_BINASCII             (1)
#define MICROPY_PY_OS                   (1)
#define MICROPY_PY_SELECT               (1)

// String features
#define MICROPY_PY_BUILTINS_STR_UNICODE (1)
#define MICROPY_PY_BUILTINS_STR_CENTER  (1)
#define MICROPY_PY_BUILTINS_STR_PARTITION (1)
#define MICROPY_PY_BUILTINS_STR_SPLITLINES (1)

// VFS support
#define MICROPY_VFS                     (1)
#define MICROPY_VFS_POSIX               (0)  // We use our own VFS
#define MICROPY_READER_VFS              (1)
#define MICROPY_READER_POSIX            (0)

// External modules
#define MICROPY_PY_SOCKET               (1)

// MayteraOS-specific modules
#define MICROPY_PY_MAYTERA              (1)
#define MICROPY_PY_MAYTERA_GUI          (1)

// Type definitions
typedef intptr_t mp_int_t;
typedef uintptr_t mp_uint_t;
typedef long mp_off_t;

// For printf
#define UINT_FMT "%lu"
#define INT_FMT "%ld"

// Keyboard interrupt
#define MICROPY_KBD_EXCEPTION           (1)

// Hooks
#define MICROPY_HOOK_POLL_MAYTERA_EVENTS mp_maytera_poll_events()
void mp_maytera_poll_events(void);

// Module registration
extern const struct _mp_obj_module_t mp_module_os;
extern const struct _mp_obj_module_t mp_module_socket;
extern const struct _mp_obj_module_t mp_module_maytera;

#define MICROPY_PORT_BUILTIN_MODULES \
    { MP_ROM_QSTR(MP_QSTR_os), MP_ROM_PTR(&mp_module_os) }, \
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&mp_module_socket) }, \
    { MP_ROM_QSTR(MP_QSTR_maytera), MP_ROM_PTR(&mp_module_maytera) }, \

// Help text
#define maytera_help_text \
    "MicroPython for MayteraOS\n" \
    "\n" \
    "Available modules:\n" \
    "  os       - File and directory operations\n" \
    "  socket   - Network socket operations\n" \
    "  maytera  - MayteraOS-specific GUI and system functions\n" \
    "\n" \
    "Type help(module) for module-specific help.\n"

// Extra built-ins
#define MICROPY_PORT_BUILTINS \

// Root pointer registration for GC
#define MICROPY_PORT_ROOT_POINTERS \
    const char *readline_hist[8];

#endif // MPCONFIGPORT_H
