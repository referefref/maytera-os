// ipc.c - Inter-Process Communication Initialization for MayteraOS

#include "ipc.h"
#include "../serial.h"

void ipc_init(void) {
    kprintf("[IPC] Initializing Inter-Process Communication...\n");

    // Initialize shared memory subsystem
    shm_init();

    // Initialize message passing subsystem
    msg_init();

    kprintf("[IPC] IPC initialization complete\n");
}

void ipc_cleanup_process(uint32_t pid) {
    // Clean up shared memory
    shm_cleanup_process(pid);

    // Clean up message channels
    msg_cleanup_process(pid);
}
