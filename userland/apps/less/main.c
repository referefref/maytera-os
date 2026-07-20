// less - simple file pager
// Usage: less [FILE]
// Controls: space/enter = next page, q = quit, /pattern = search
#include "stdlib.h"
#include "stdio.h"
#include "string.h"
#include "unistd.h"
#include "fcntl.h"
#include "errno.h"

#define BUF_SIZE   65536
#define PAGE_LINES 23

static char buf[BUF_SIZE];
static int buf_len = 0;

// Line index: start offsets of each line in buf
static int line_off[8192];
static int line_count = 0;

static void index_lines(void) {
    line_count = 0;
    if (buf_len == 0) return;
    line_off[line_count++] = 0;
    for (int i = 0; i < buf_len; i++) {
        if (buf[i] == '\n' && i + 1 < buf_len && line_count < 8192) {
            line_off[line_count++] = i + 1;
        }
    }
}

static void print_line(int idx) {
    if (idx < 0 || idx >= line_count) return;
    int start = line_off[idx];
    int end = (idx + 1 < line_count) ? line_off[idx + 1] : buf_len;
    write(1, buf + start, end - start);
    // Ensure newline
    if (end > start && buf[end - 1] != '\n') write(1, "\n", 1);
}

static void print_page(int start_line) {
    for (int i = 0; i < PAGE_LINES && start_line + i < line_count; i++) {
        print_line(start_line + i);
    }
}

static void show_status(int cur_line) {
    // Inverse video status line
    printf("\033[7m -- less: line %d/%d (press SPACE for more, q to quit) --\033[0m", 
           cur_line + 1, line_count);
}

// Simple substring search
static int find_in_line(int line_idx, const char *pattern) {
    if (line_idx < 0 || line_idx >= line_count) return 0;
    int start = line_off[line_idx];
    int end = (line_idx + 1 < line_count) ? line_off[line_idx + 1] : buf_len;
    int plen = strlen(pattern);
    if (plen == 0) return 0;
    for (int i = start; i <= end - plen; i++) {
        int match = 1;
        for (int j = 0; j < plen; j++) {
            if (buf[i + j] != pattern[j]) { match = 0; break; }
        }
        if (match) return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    int fd = 0;  // default: stdin
    if (argc > 1) {
        char path[512];
        const char *target = argv[1];
        if (argv[1][0] != '/') {
            char cwd_buf[256];
            if (!getcwd(cwd_buf, sizeof(cwd_buf))) cwd_buf[0] = 0;
            int j = 0;
            for (int i = 0; cwd_buf[i] && j < 510; i++) path[j++] = cwd_buf[i];
            if (j > 0 && path[j-1] != '/' && j < 511) path[j++] = '/';
            for (int k = 0; argv[1][k] && j < 511; k++) path[j++] = argv[1][k];
            path[j] = 0;
            target = path;
        }
        fd = open(target, O_RDONLY);
        if (fd < 0) {
            perror(argv[1]);
            return 1;
        }
    }

    // Read file
    long n;
    while (buf_len < BUF_SIZE - 1) {
        n = read(fd, buf + buf_len, BUF_SIZE - 1 - buf_len);
        if (n <= 0) break;
        buf_len += n;
    }
    buf[buf_len] = '\0';
    if (fd > 0) close(fd);

    if (buf_len == 0) return 0;

    index_lines();

    // If content fits on one screen, just print it
    if (line_count <= PAGE_LINES) {
        write(1, buf, buf_len);
        return 0;
    }

    // Interactive paging
    int cur_line = 0;
    print_page(cur_line);
    show_status(cur_line);

    while (1) {
        int c = getchar();
        if (c < 0 || c == 'q' || c == 'Q') {
            printf("\r\033[K");  // Clear status line
            break;
        }
        if (c == ' ' || c == 'f') {
            // Next page
            cur_line += PAGE_LINES;
            if (cur_line >= line_count) cur_line = line_count - 1;
            printf("\r\033[K");
            print_page(cur_line);
            show_status(cur_line);
        } else if (c == '\n' || c == '\r' || c == 'j') {
            // Next line
            if (cur_line < line_count - 1) {
                cur_line++;
                printf("\r\033[K");
                print_line(cur_line + PAGE_LINES - 1);
                show_status(cur_line);
            }
        } else if (c == 'b') {
            // Previous page
            cur_line -= PAGE_LINES;
            if (cur_line < 0) cur_line = 0;
            printf("\r\033[K\033[2J\033[H");
            print_page(cur_line);
            show_status(cur_line);
        } else if (c == '/') {
            // Search forward
            printf("\r\033[K/");
            char pattern[128];
            int pi = 0;
            while (pi < 127) {
                int sc = getchar();
                if (sc == '\n' || sc == '\r') break;
                if (sc == '\b' || sc == 127) {
                    if (pi > 0) { pi--; printf("\b \b"); }
                    continue;
                }
                if (sc >= ' ' && sc < 127) {
                    pattern[pi++] = (char)sc;
                    putchar(sc);
                }
            }
            pattern[pi] = '\0';
            // Find next occurrence
            for (int i = cur_line + 1; i < line_count; i++) {
                if (find_in_line(i, pattern)) {
                    cur_line = i;
                    printf("\r\033[K\033[2J\033[H");
                    print_page(cur_line);
                    show_status(cur_line);
                    break;
                }
            }
        } else if (c == 'g') {
            // Go to beginning
            cur_line = 0;
            printf("\r\033[K\033[2J\033[H");
            print_page(cur_line);
            show_status(cur_line);
        } else if (c == 'G') {
            // Go to end
            cur_line = line_count > PAGE_LINES ? line_count - PAGE_LINES : 0;
            printf("\r\033[K\033[2J\033[H");
            print_page(cur_line);
            show_status(cur_line);
        }
    }

    return 0;
}
