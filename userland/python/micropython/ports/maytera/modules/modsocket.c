// modsocket.c - Socket module for MayteraOS MicroPython
// Provides network socket operations

#include "py/runtime.h"
#include "py/obj.h"
#include "py/objstr.h"
#include "py/stream.h"

// Network syscalls (must match kernel)
#define SYS_SOCKET      60
#define SYS_CONNECT     61
#define SYS_SEND        62
#define SYS_RECV        63
#define SYS_BIND        64
#define SYS_LISTEN      65
#define SYS_ACCEPT      66
#define SYS_CLOSE       11

// Socket types
#define SOCK_STREAM     1
#define SOCK_DGRAM      2

// Address families
#define AF_INET         2
#define AF_INET6        10

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

static inline long syscall4(long num, long a1, long a2, long a3, long a4) {
    long result;
    register long r10 asm("r10") = a4;
    asm volatile("syscall" : "=a"(result) 
                 : "a"(num), "D"(a1), "S"(a2), "d"(a3), "r"(r10) 
                 : "rcx", "r11", "memory");
    return result;
}

// IPv4 address structure
typedef struct {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
} sockaddr_in_t;

// ============================================================================
// Socket Class
// ============================================================================

typedef struct _socket_obj_t {
    mp_obj_base_t base;
    int fd;
    int family;
    int type;
} socket_obj_t;

const mp_obj_type_t socket_type;

// Parse IP address string to uint32_t
static uint32_t parse_ip(const char *ip) {
    uint32_t result = 0;
    int octet = 0;
    int shift = 0;
    
    while (*ip) {
        if (*ip >= '0' && *ip <= '9') {
            octet = octet * 10 + (*ip - '0');
        } else if (*ip == '.') {
            result |= (octet & 0xFF) << shift;
            shift += 8;
            octet = 0;
        }
        ip++;
    }
    result |= (octet & 0xFF) << shift;
    return result;
}

// Socket constructor: socket(family, type)
STATIC mp_obj_t socket_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 2, false);
    
    int family = n_args > 0 ? mp_obj_get_int(args[0]) : AF_INET;
    int sock_type = n_args > 1 ? mp_obj_get_int(args[1]) : SOCK_STREAM;
    
    // Create socket via syscall
    int fd = (int)syscall2(SYS_SOCKET, family, sock_type);
    if (fd < 0) {
        mp_raise_OSError(-fd);
    }
    
    socket_obj_t *self = mp_obj_malloc(socket_obj_t, type);
    self->fd = fd;
    self->family = family;
    self->type = sock_type;
    
    return MP_OBJ_FROM_PTR(self);
}

// socket.connect((host, port))
STATIC mp_obj_t socket_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    // Parse (host, port) tuple
    mp_obj_t *items;
    size_t len;
    mp_obj_tuple_get(addr_in, &len, &items);
    if (len != 2) {
        mp_raise_ValueError("address must be (host, port) tuple");
    }
    
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);
    
    // Build sockaddr_in
    sockaddr_in_t addr = {0};
    addr.family = AF_INET;
    addr.port = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);  // htons
    addr.addr = parse_ip(host);
    
    int result = (int)syscall3(SYS_CONNECT, self->fd, (long)&addr, sizeof(addr));
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_connect_obj, socket_connect);

// socket.send(data) -> int
STATIC mp_obj_t socket_send(mp_obj_t self_in, mp_obj_t data_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(data_in, &bufinfo, MP_BUFFER_READ);
    
    long n = syscall3(SYS_SEND, self->fd, (long)bufinfo.buf, bufinfo.len);
    if (n < 0) {
        mp_raise_OSError(-n);
    }
    return mp_obj_new_int(n);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_send_obj, socket_send);

// socket.recv(bufsize) -> bytes
STATIC mp_obj_t socket_recv(mp_obj_t self_in, mp_obj_t bufsize_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    size_t bufsize = mp_obj_get_int(bufsize_in);
    
    vstr_t vstr;
    vstr_init_len(&vstr, bufsize);
    
    long n = syscall3(SYS_RECV, self->fd, (long)vstr.buf, bufsize);
    if (n < 0) {
        vstr_clear(&vstr);
        mp_raise_OSError(-n);
    }
    
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_recv_obj, socket_recv);

// socket.bind((host, port))
STATIC mp_obj_t socket_bind(mp_obj_t self_in, mp_obj_t addr_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_obj_t *items;
    size_t len;
    mp_obj_tuple_get(addr_in, &len, &items);
    if (len != 2) {
        mp_raise_ValueError("address must be (host, port) tuple");
    }
    
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);
    
    sockaddr_in_t addr = {0};
    addr.family = AF_INET;
    addr.port = ((port & 0xFF) << 8) | ((port >> 8) & 0xFF);
    if (host[0] == '0' && host[1] == '.' && host[2] == '0') {
        addr.addr = 0;  // INADDR_ANY
    } else {
        addr.addr = parse_ip(host);
    }
    
    int result = (int)syscall3(SYS_BIND, self->fd, (long)&addr, sizeof(addr));
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(socket_bind_obj, socket_bind);

// socket.listen([backlog])
STATIC mp_obj_t socket_listen(size_t n_args, const mp_obj_t *args) {
    socket_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    int backlog = n_args > 1 ? mp_obj_get_int(args[1]) : 5;
    
    int result = (int)syscall2(SYS_LISTEN, self->fd, backlog);
    if (result < 0) {
        mp_raise_OSError(-result);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_listen_obj, 1, 2, socket_listen);

// socket.accept() -> (socket, addr)
STATIC mp_obj_t socket_accept(mp_obj_t self_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    
    sockaddr_in_t addr = {0};
    int addrlen = sizeof(addr);
    
    int new_fd = (int)syscall3(SYS_ACCEPT, self->fd, (long)&addr, (long)&addrlen);
    if (new_fd < 0) {
        mp_raise_OSError(-new_fd);
    }
    
    // Create new socket object
    socket_obj_t *new_sock = mp_obj_malloc(socket_obj_t, &socket_type);
    new_sock->fd = new_fd;
    new_sock->family = self->family;
    new_sock->type = self->type;
    
    // Format address
    uint32_t ip = addr.addr;
    uint16_t port = ((addr.port & 0xFF) << 8) | ((addr.port >> 8) & 0xFF);
    
    char ip_str[16];
    int len = 0;
    for (int i = 0; i < 4; i++) {
        int octet = (ip >> (i * 8)) & 0xFF;
        if (octet >= 100) ip_str[len++] = '0' + octet / 100;
        if (octet >= 10) ip_str[len++] = '0' + (octet / 10) % 10;
        ip_str[len++] = '0' + octet % 10;
        if (i < 3) ip_str[len++] = '.';
    }
    ip_str[len] = 0;
    
    mp_obj_t addr_tuple[2] = {
        mp_obj_new_str(ip_str, len),
        mp_obj_new_int(port)
    };
    
    mp_obj_t result[2] = {
        MP_OBJ_FROM_PTR(new_sock),
        mp_obj_new_tuple(2, addr_tuple)
    };
    
    return mp_obj_new_tuple(2, result);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_accept_obj, socket_accept);

// socket.close()
STATIC mp_obj_t socket_close(mp_obj_t self_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd >= 0) {
        syscall1(SYS_CLOSE, self->fd);
        self->fd = -1;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_close_obj, socket_close);

// socket.fileno() -> int
STATIC mp_obj_t socket_fileno(mp_obj_t self_in) {
    socket_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->fd);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(socket_fileno_obj, socket_fileno);

// Socket methods table
STATIC const mp_rom_map_elem_t socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect), MP_ROM_PTR(&socket_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&socket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv), MP_ROM_PTR(&socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind), MP_ROM_PTR(&socket_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen), MP_ROM_PTR(&socket_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept), MP_ROM_PTR(&socket_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_close), MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno), MP_ROM_PTR(&socket_fileno_obj) },
};
STATIC MP_DEFINE_CONST_DICT(socket_locals_dict, socket_locals_dict_table);

MP_DEFINE_CONST_OBJ_TYPE(
    socket_type,
    MP_QSTR_socket,
    MP_TYPE_FLAG_NONE,
    make_new, socket_make_new,
    locals_dict, &socket_locals_dict
);

// ============================================================================
// Module-level functions
// ============================================================================

// socket.getaddrinfo(host, port) -> list
STATIC mp_obj_t mod_socket_getaddrinfo(mp_obj_t host_in, mp_obj_t port_in) {
    const char *host = mp_obj_str_get_str(host_in);
    int port = mp_obj_get_int(port_in);
    
    // Simple implementation - just return the parsed IP
    uint32_t ip = parse_ip(host);
    
    mp_obj_t items[5] = {
        mp_obj_new_int(AF_INET),
        mp_obj_new_int(SOCK_STREAM),
        mp_obj_new_int(0),
        mp_obj_new_str("", 0),
        mp_obj_new_tuple(2, (mp_obj_t[]){host_in, mp_obj_new_int(port)})
    };
    
    mp_obj_t result = mp_obj_new_list(1, &mp_obj_new_tuple(5, items));
    return result;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(mod_socket_getaddrinfo_obj, mod_socket_getaddrinfo);

// ============================================================================
// Module Definition
// ============================================================================

STATIC const mp_rom_map_elem_t mp_module_socket_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_socket) },
    
    // Socket class
    { MP_ROM_QSTR(MP_QSTR_socket), MP_ROM_PTR(&socket_type) },
    
    // Functions
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&mod_socket_getaddrinfo_obj) },
    
    // Socket types
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(SOCK_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_DGRAM), MP_ROM_INT(SOCK_DGRAM) },
    
    // Address families
    { MP_ROM_QSTR(MP_QSTR_AF_INET), MP_ROM_INT(AF_INET) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET6), MP_ROM_INT(AF_INET6) },
};
STATIC MP_DEFINE_CONST_DICT(mp_module_socket_globals, mp_module_socket_globals_table);

const mp_obj_module_t mp_module_socket = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_socket_globals,
};

MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);
