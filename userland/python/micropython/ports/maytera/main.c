// main.c - MicroPython main entry point for MayteraOS
// This is the Python interpreter executable

#include "py/compile.h"
#include "py/runtime.h"
#include "py/repl.h"
#include "py/gc.h"
#include "py/mperrno.h"
#include "py/stackctrl.h"
#include "py/mphal.h"

// Heap for MicroPython
static char heap[64 * 1024];  // 64KB heap

// Simple string functions
static int str_len(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

static int str_starts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

// Execute Python code from string
static int execute_from_string(const char *code) {
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lex = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_, code, str_len(code), false);
        mp_parse_tree_t pt = mp_parse(lex, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&pt, lex->source_name, false);
        mp_call_function_0(module_fun);
        nlr_pop();
        return 0;
    } else {
        // Exception
        mp_obj_print_exception(&mp_plat_print, MP_OBJ_FROM_PTR(nlr.ret_val));
        return 1;
    }
}

// Execute Python file
static int execute_file(const char *filename) {
    // Syscall to open file
    long fd;
    asm volatile(
        "mov $10, %%rax\n"  // SYS_OPEN
        "mov %1, %%rdi\n"
        "xor %%esi, %%esi\n" // O_RDONLY
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(fd)
        : "r"(filename)
        : "rax", "rdi", "rsi", "rcx", "r11", "memory"
    );
    
    if (fd < 0) {
        mp_hal_stdout_tx_str("Error: Cannot open file: ");
        mp_hal_stdout_tx_str(filename);
        mp_hal_stdout_tx_str("\n");
        return 1;
    }
    
    // Read file into buffer
    static char code_buf[32 * 1024];  // 32KB max script
    long n;
    asm volatile(
        "mov $12, %%rax\n"  // SYS_READ
        "mov %1, %%rdi\n"
        "mov %2, %%rsi\n"
        "mov $32767, %%rdx\n"
        "syscall\n"
        "mov %%rax, %0\n"
        : "=r"(n)
        : "r"(fd), "r"((long)code_buf)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    
    // Close file
    asm volatile(
        "mov $11, %%rax\n"  // SYS_CLOSE
        "mov %0, %%rdi\n"
        "syscall\n"
        :
        : "r"(fd)
        : "rax", "rdi", "rcx", "r11", "memory"
    );
    
    if (n <= 0) {
        mp_hal_stdout_tx_str("Error: Cannot read file\n");
        return 1;
    }
    
    code_buf[n] = '\0';
    return execute_from_string(code_buf);
}

// REPL (Read-Eval-Print Loop)
static void run_repl(void) {
    mp_hal_stdout_tx_str("MicroPython for MayteraOS\n");
    mp_hal_stdout_tx_str("Type \"help()\" for more information.\n");
    
    static char line_buf[256];
    int line_pos = 0;
    int indent_level = 0;
    static char block_buf[4096];
    int block_pos = 0;
    
    mp_hal_stdout_tx_str(">>> ");
    
    while (1) {
        int c = mp_hal_stdin_rx_chr();
        
        if (c == '\r' || c == '\n') {
            mp_hal_stdout_tx_str("\r\n");
            line_buf[line_pos] = '\0';
            
            // Check for block continuation
            int needs_more = 0;
            if (line_pos > 0 && line_buf[line_pos - 1] == ':') {
                needs_more = 1;
                indent_level++;
            }
            
            // Check for continuation of multi-line
            if (indent_level > 0) {
                // Count leading spaces
                int spaces = 0;
                while (line_buf[spaces] == ' ') spaces++;
                
                if (spaces == 0 && line_pos == 0) {
                    // Empty line ends block
                    indent_level = 0;
                    block_buf[block_pos] = '\0';
                    execute_from_string(block_buf);
                    block_pos = 0;
                } else {
                    // Add to block
                    for (int i = 0; i < line_pos; i++) {
                        if (block_pos < (int)sizeof(block_buf) - 2) {
                            block_buf[block_pos++] = line_buf[i];
                        }
                    }
                    block_buf[block_pos++] = '\n';
                }
            } else if (needs_more) {
                // Start new block
                for (int i = 0; i < line_pos; i++) {
                    if (block_pos < (int)sizeof(block_buf) - 2) {
                        block_buf[block_pos++] = line_buf[i];
                    }
                }
                block_buf[block_pos++] = '\n';
            } else if (line_pos > 0) {
                // Single line - execute immediately
                execute_from_string(line_buf);
            }
            
            line_pos = 0;
            
            if (indent_level > 0) {
                mp_hal_stdout_tx_str("... ");
            } else {
                mp_hal_stdout_tx_str(">>> ");
            }
        } else if (c == '\b' || c == 127) {
            // Backspace
            if (line_pos > 0) {
                line_pos--;
                mp_hal_stdout_tx_str("\b \b");
            }
        } else if (c == 3) {
            // Ctrl+C
            mp_hal_stdout_tx_str("\r\nKeyboardInterrupt\n>>> ");
            line_pos = 0;
            indent_level = 0;
            block_pos = 0;
        } else if (c == 4) {
            // Ctrl+D - exit
            mp_hal_stdout_tx_str("\r\n");
            break;
        } else if (c >= ' ' && c < 127) {
            // Printable character
            if (line_pos < (int)sizeof(line_buf) - 1) {
                line_buf[line_pos++] = c;
                char s[2] = {c, 0};
                mp_hal_stdout_tx_str(s);
            }
        }
    }
}

// Print usage
static void print_usage(const char *prog) {
    mp_hal_stdout_tx_str("Usage: ");
    mp_hal_stdout_tx_str(prog);
    mp_hal_stdout_tx_str(" [options] [script.py] [args...]\n");
    mp_hal_stdout_tx_str("\nOptions:\n");
    mp_hal_stdout_tx_str("  -c CODE     Execute CODE\n");
    mp_hal_stdout_tx_str("  -h, --help  Show this help\n");
    mp_hal_stdout_tx_str("  -V          Show version\n");
    mp_hal_stdout_tx_str("\nWithout arguments, start interactive REPL.\n");
}

// Main entry point
int main(int argc, char **argv) {
    // Initialize MicroPython
    mp_stack_ctrl_init();
    mp_stack_set_limit(40000);
    
    gc_init(heap, heap + sizeof(heap));
    mp_init();
    
    // Parse arguments
    int result = 0;
    
    if (argc == 1) {
        // No arguments - run REPL
        run_repl();
    } else if (str_eq(argv[1], "-h") || str_eq(argv[1], "--help")) {
        print_usage(argv[0]);
    } else if (str_eq(argv[1], "-V") || str_eq(argv[1], "--version")) {
        mp_hal_stdout_tx_str("MicroPython 1.20.0 for MayteraOS\n");
    } else if (str_eq(argv[1], "-c") && argc > 2) {
        // Execute code from command line
        result = execute_from_string(argv[2]);
    } else if (argv[1][0] != '-') {
        // Execute file
        result = execute_file(argv[1]);
    } else {
        mp_hal_stdout_tx_str("Unknown option: ");
        mp_hal_stdout_tx_str(argv[1]);
        mp_hal_stdout_tx_str("\n");
        print_usage(argv[0]);
        result = 1;
    }
    
    // Cleanup
    gc_sweep_all();
    mp_deinit();
    
    return result;
}

// Required by MicroPython - handles fatal errors
void nlr_jump_fail(void *val) {
    mp_hal_stdout_tx_str("FATAL: nlr_jump_fail\n");
    while (1) {}
}

// Required by MicroPython - handles uncaught exceptions
void __assert_func(const char *file, int line, const char *func, const char *expr) {
    mp_hal_stdout_tx_str("ASSERT FAIL: ");
    mp_hal_stdout_tx_str(expr);
    mp_hal_stdout_tx_str("\n");
    while (1) {}
}
