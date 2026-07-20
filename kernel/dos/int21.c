// int21.c - DOS INT 21h Services Implementation
// Implements the most common DOS INT 21h functions
#include "dos.h"
#include "../serial.h"
#include "../mm/heap.h"
#include "../string.h"

// External references to dos.c functions and state
extern dos_state_t *dos_get_state(void);
extern char dos_getchar(void);
extern void dos_putchar(char c);
extern void dos_print_string(const char *str);
extern int dos_create(const char *path, uint16_t attr);
extern int dos_open(const char *path, uint8_t mode);
extern int dos_close(int handle);
extern int dos_read(int handle, void *buf, uint16_t len, uint16_t *bytes_read);
extern int dos_write(int handle, const void *buf, uint16_t len, uint16_t *bytes_written);
extern void dos_terminate(uint8_t return_code);

// Forward declarations for internal functions
static int int21_char_input_echo(dos_regs_t *regs);
static int int21_char_output(dos_regs_t *regs);
static int int21_print_string(dos_regs_t *regs);
static int int21_create_file(dos_regs_t *regs);
static int int21_open_file(dos_regs_t *regs);
static int int21_close_file(dos_regs_t *regs);
static int int21_read_file(dos_regs_t *regs);
static int int21_write_file(dos_regs_t *regs);
static int int21_terminate(dos_regs_t *regs);

// INT 21h Function 01h - Character input with echo
// Input: None
// Output: AL = character read
static int int21_char_input_echo(dos_regs_t *regs) {
    char c = dos_getchar();
    DOS_SET_AL(regs, (uint8_t)c);
    return 0;
}

// INT 21h Function 02h - Character output
// Input: DL = character to output
// Output: AL = last character output (DL)
static int int21_char_output(dos_regs_t *regs) {
    char c = (char)DOS_DL(regs);
    dos_putchar(c);
    DOS_SET_AL(regs, (uint8_t)c);
    return 0;
}

// INT 21h Function 09h - Print string
// Input: DS:DX = pointer to '$' terminated string
// Output: AL = '$'
static int int21_print_string(dos_regs_t *regs) {
    // In our flat memory model, DS:DX forms an address
    // We use DS << 4 + DX to get the linear address
    void *str_ptr = dos_segofs_to_ptr(regs->ds, regs->dx);

    dos_print_string((const char *)str_ptr);
    DOS_SET_AL(regs, '$');
    return 0;
}

// INT 21h Function 3Ch - Create file
// Input: CX = file attributes
//        DS:DX = pointer to ASCIIZ filename
// Output: If CF=0, AX = file handle
//         If CF=1, AX = error code
static int int21_create_file(dos_regs_t *regs) {
    void *path_ptr = dos_segofs_to_ptr(regs->ds, regs->dx);
    uint16_t attr = regs->cx;

    int handle = dos_create((const char *)path_ptr, attr);

    if (handle >= 0) {
        regs->ax = (uint16_t)handle;
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = dos_get_error();
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 3Dh - Open file
// Input: AL = access mode (0=read, 1=write, 2=read/write)
//        DS:DX = pointer to ASCIIZ filename
// Output: If CF=0, AX = file handle
//         If CF=1, AX = error code
static int int21_open_file(dos_regs_t *regs) {
    void *path_ptr = dos_segofs_to_ptr(regs->ds, regs->dx);
    uint8_t mode = DOS_AL(regs);

    int handle = dos_open((const char *)path_ptr, mode);

    if (handle >= 0) {
        regs->ax = (uint16_t)handle;
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = dos_get_error();
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 3Eh - Close file
// Input: BX = file handle
// Output: If CF=0, success
//         If CF=1, AX = error code
static int int21_close_file(dos_regs_t *regs) {
    int handle = (int)regs->bx;

    int result = dos_close(handle);

    if (result == 0) {
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = (uint16_t)result;
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 3Fh - Read from file
// Input: BX = file handle
//        CX = number of bytes to read
//        DS:DX = buffer address
// Output: If CF=0, AX = number of bytes read
//         If CF=1, AX = error code
static int int21_read_file(dos_regs_t *regs) {
    int handle = (int)regs->bx;
    uint16_t count = regs->cx;
    void *buffer = dos_segofs_to_ptr(regs->ds, regs->dx);

    uint16_t bytes_read = 0;
    int result = dos_read(handle, buffer, count, &bytes_read);

    if (result == 0) {
        regs->ax = bytes_read;
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = (uint16_t)result;
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 40h - Write to file
// Input: BX = file handle
//        CX = number of bytes to write
//        DS:DX = buffer address
// Output: If CF=0, AX = number of bytes written
//         If CF=1, AX = error code
static int int21_write_file(dos_regs_t *regs) {
    int handle = (int)regs->bx;
    uint16_t count = regs->cx;
    void *buffer = dos_segofs_to_ptr(regs->ds, regs->dx);

    uint16_t bytes_written = 0;
    int result = dos_write(handle, buffer, count, &bytes_written);

    if (result == 0) {
        regs->ax = bytes_written;
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = (uint16_t)result;
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 4Ch - Terminate program with return code
// Input: AL = return code
// Output: None (does not return)
static int int21_terminate(dos_regs_t *regs) {
    uint8_t return_code = DOS_AL(regs);
    dos_terminate(return_code);
    return 0;
}

// Main INT 21h dispatcher
// Dispatches to appropriate handler based on function number in AH
int int21_dispatch(uint8_t function, dos_regs_t *regs) {
    // Set function number in AH for handlers that need it
    DOS_SET_AH(regs, function);

    switch (function) {
        case DOS_CHAR_INPUT_ECHO:   // 01h
            return int21_char_input_echo(regs);

        case DOS_CHAR_OUTPUT:       // 02h
            return int21_char_output(regs);

        case DOS_PRINT_STRING:      // 09h
            return int21_print_string(regs);

        case DOS_CREATE_FILE:       // 3Ch
            return int21_create_file(regs);

        case DOS_OPEN_FILE:         // 3Dh
            return int21_open_file(regs);

        case DOS_CLOSE_FILE:        // 3Eh
            return int21_close_file(regs);

        case DOS_READ_FILE:         // 3Fh
            return int21_read_file(regs);

        case DOS_WRITE_FILE:        // 40h
            return int21_write_file(regs);

        case DOS_TERMINATE:         // 4Ch
            return int21_terminate(regs);

        default:
            // Unknown function
            kprintf("DOS: Unknown INT 21h function: %02Xh\n", function);
            regs->ax = DOS_ERR_INVALID_FUNC;
            DOS_SET_CARRY(regs);
            return -1;
    }
}

// Additional commonly used INT 21h functions (not requested but useful)

// INT 21h Function 00h - Terminate program
// (older method, use 4Ch instead)
int int21_terminate_old(dos_regs_t *regs) {
    UNUSED(regs);
    dos_terminate(0);
    return 0;
}

// INT 21h Function 06h - Direct console I/O
// Input: DL = character to output (if DL != FFh)
//        DL = FFh to read character
// Output: AL = character read (if DL was FFh)
//         ZF = 1 if no character available
int int21_direct_console_io(dos_regs_t *regs) {
    uint8_t dl = DOS_DL(regs);

    if (dl == 0xFF) {
        // Input
        if (serial_received(COM1)) {
            char c = serial_read(COM1);
            DOS_SET_AL(regs, (uint8_t)c);
            regs->flags &= ~0x40;  // Clear ZF
        } else {
            DOS_SET_AL(regs, 0);
            regs->flags |= 0x40;   // Set ZF
        }
    } else {
        // Output
        dos_putchar((char)dl);
        DOS_SET_AL(regs, dl);
    }

    return 0;
}

// INT 21h Function 08h - Character input without echo
int int21_char_input_no_echo(dos_regs_t *regs) {
    // Wait for input
    while (!serial_received(COM1)) {
        // Could add timeout here
    }
    char c = serial_read(COM1);
    DOS_SET_AL(regs, (uint8_t)c);
    return 0;
}

// INT 21h Function 0Ah - Buffered input
// Input: DS:DX = pointer to input buffer
//        First byte of buffer = max chars to read
// Output: Second byte = actual chars read
//         Chars follow starting at offset 2
int int21_buffered_input(dos_regs_t *regs) {
    uint8_t *buffer = (uint8_t *)dos_segofs_to_ptr(regs->ds, regs->dx);
    uint8_t max_len = buffer[0];
    uint8_t count = 0;

    // Read characters until CR
    while (count < max_len) {
        char c = dos_getchar();

        if (c == '\r' || c == '\n') {
            break;
        }

        if (c == 0x08 && count > 0) {  // Backspace
            count--;
            dos_putchar(' ');
            dos_putchar(0x08);
            continue;
        }

        buffer[2 + count] = (uint8_t)c;
        count++;
    }

    buffer[1] = count;
    buffer[2 + count] = '\r';  // Terminate with CR

    return 0;
}

// INT 21h Function 25h - Set interrupt vector
// Input: AL = interrupt number
//        DS:DX = new interrupt handler address
int int21_set_vector(dos_regs_t *regs) {
    // In our emulation, we don't actually modify interrupt vectors
    // Just acknowledge the request
    UNUSED(regs);
    kprintf("DOS: Set interrupt vector (ignored in emulation)\n");
    return 0;
}

// INT 21h Function 35h - Get interrupt vector
// Input: AL = interrupt number
// Output: ES:BX = current interrupt handler address
int int21_get_vector(dos_regs_t *regs) {
    // Return a dummy value
    uint8_t int_num = DOS_AL(regs);
    UNUSED(int_num);
    regs->es = 0;
    regs->bx = 0;
    return 0;
}

// INT 21h Function 48h - Allocate memory
// Input: BX = number of 16-byte paragraphs to allocate
// Output: If CF=0, AX = segment of allocated block
//         If CF=1, AX = error code, BX = max available paragraphs
int int21_alloc_memory(dos_regs_t *regs) {
    uint16_t paragraphs = regs->bx;
    dos_state_t *state = dos_get_state();

    void *ptr = kmalloc((size_t)paragraphs * 16);
    if (ptr) {
        // Return segment (simplified - just use address as segment)
        uint32_t segofs = dos_ptr_to_segofs(ptr);
        regs->ax = (uint16_t)(segofs >> 16);  // Segment
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = DOS_ERR_NO_MEMORY;
        regs->bx = (uint16_t)(state->dos_memory_size / 16);  // Max available
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 49h - Free memory
// Input: ES = segment of block to free
// Output: If CF=0, success
//         If CF=1, AX = error code
int int21_free_memory(dos_regs_t *regs) {
    void *ptr = dos_segofs_to_ptr(regs->es, 0);

    if (ptr) {
        kfree(ptr);
        DOS_CLEAR_CARRY(regs);
        return 0;
    } else {
        regs->ax = DOS_ERR_INVALID_ACCESS;
        DOS_SET_CARRY(regs);
        return -1;
    }
}

// INT 21h Function 4Ah - Resize memory block
// Input: ES = segment of block to resize
//        BX = new size in paragraphs
// Output: If CF=0, success
//         If CF=1, AX = error code, BX = max available
int int21_resize_memory(dos_regs_t *regs) {
    void *ptr = dos_segofs_to_ptr(regs->es, 0);
    uint16_t paragraphs = regs->bx;

    if (ptr) {
        void *new_ptr = krealloc(ptr, (size_t)paragraphs * 16);
        if (new_ptr) {
            DOS_CLEAR_CARRY(regs);
            return 0;
        }
    }

    regs->ax = DOS_ERR_NO_MEMORY;
    DOS_SET_CARRY(regs);
    return -1;
}

// INT 21h Function 19h - Get current drive
// Output: AL = current drive (0=A, 1=B, 2=C, etc.)
int int21_get_drive(dos_regs_t *regs) {
    dos_state_t *state = dos_get_state();
    DOS_SET_AL(regs, state->current_drive);
    return 0;
}

// INT 21h Function 0Eh - Set current drive
// Input: DL = new drive (0=A, 1=B, 2=C, etc.)
// Output: AL = number of available drives
int int21_set_drive(dos_regs_t *regs) {
    dos_state_t *state = dos_get_state();
    state->current_drive = DOS_DL(regs);
    DOS_SET_AL(regs, 3);  // Report A:, B:, C: available
    return 0;
}

// INT 21h Function 30h - Get DOS version
// Output: AL = major version, AH = minor version
//         BL:CX = 24-bit serial number
//         BH = OEM ID
int int21_get_version(dos_regs_t *regs) {
    // Report as DOS 6.22
    DOS_SET_AL(regs, 6);   // Major version
    DOS_SET_AH(regs, 22);  // Minor version
    regs->bx = 0;
    regs->cx = 0;
    return 0;
}

// INT 21h Function 2Fh - Get DTA address
// Output: ES:BX = current DTA address
int int21_get_dta(dos_regs_t *regs) {
    dos_state_t *state = dos_get_state();
    uint32_t segofs = dos_ptr_to_segofs(state->dta);
    regs->es = (uint16_t)(segofs >> 16);
    regs->bx = (uint16_t)(segofs & 0xFFFF);
    return 0;
}

// INT 21h Function 1Ah - Set DTA address
// Input: DS:DX = new DTA address
int int21_set_dta(dos_regs_t *regs) {
    dos_state_t *state = dos_get_state();
    state->dta = dos_segofs_to_ptr(regs->ds, regs->dx);
    return 0;
}
