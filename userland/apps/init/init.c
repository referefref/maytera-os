// init.c - Init process for MayteraOS
// First userland process - starts compositor and system services

#include "../../libc/syscall.h"
#include "../../libc/stdio.h"
#include "../../libc/string.h"

// ============================================================================
// Service Definitions
// ============================================================================

typedef struct {
    const char *name;
    const char *path;
    int pid;
    int required;      // If true, system cannot function without it
    int started;
} service_t;

static service_t services[] = {
    {"compositor",  "/apps/compositor",    0, 1,  0},
    {"desktop",     "/apps/desktop",       0, 0, 0},
    {"taskbar",     "/apps/taskbar",       0, 0, 0},
    {NULL, NULL, 0, 0, 0}
};

// ============================================================================
// Service Management
// ============================================================================

static int start_service(service_t *svc) {
    printf("[init] Starting %s...\n", svc->name);
    
    int pid = sys_fork();
    if (pid == 0) {
        // Child process
        char *argv[] = {(char *)svc->path, NULL};
        execv(svc->path, argv);
        
        // exec failed
        printf("[init] ERROR: Failed to exec %s\n", svc->path);
        sys_exit(1);
    } else if (pid > 0) {
        svc->pid = pid;
        svc->started = 1;
        printf("[init] Started %s (pid %d)\n", svc->name, pid);
        return 0;
    } else {
        printf("[init] ERROR: Failed to fork for %s\n", svc->name);
        return -1;
    }
}

static void start_all_services(void) {
    for (int i = 0; services[i].name != NULL; i++) {
        if (services[i].required && !services[i].started) {
            if (start_service(&services[i]) < 0 && services[i].required) {
                printf("[init] FATAL: Required service %s failed to start\n", 
                       services[i].name);
            }
        }
    }
}

static void wait_for_children(void) {
    while (1) {
        sys_sleep(1000);
        
        for (int i = 0; services[i].name != NULL; i++) {
            if (services[i].required && services[i].started) {
                // Check if process is still alive and restart if needed
            }
        }
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char **argv) {
    printf("\n");
    printf("================================\n");
    printf("  MayteraOS Init Process\n");
    printf("================================\n");
    printf("\n");
    
    printf("[init] PID = %d\n", sys_getpid());
    
    sys_sleep(100);
    
    printf("[init] Starting graphical environment...\n");
    start_all_services();
    
    sys_sleep(500);
    
    printf("[init] System ready\n");
    
    wait_for_children();
    
    printf("[init] ERROR: Init exiting unexpectedly!\n");
    return 1;
}
