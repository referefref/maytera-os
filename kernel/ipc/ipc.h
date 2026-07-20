// ipc.h - Inter-Process Communication for MayteraOS
// Main header that includes all IPC subsystems
#ifndef IPC_H
#define IPC_H

#include "shm.h"
#include "msg.h"

/**
 * Initialize all IPC subsystems
 * Called once during kernel startup
 */
void ipc_init(void);

/**
 * Clean up all IPC resources for a process
 * Called when a process terminates
 * @param pid   Process ID being cleaned up
 */
void ipc_cleanup_process(uint32_t pid);

#endif // IPC_H
