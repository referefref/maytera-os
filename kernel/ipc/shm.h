// shm.h - Shared Memory IPC for MayteraOS
// Implements sys_shm_create, sys_shm_map, sys_shm_unmap, sys_shm_destroy
#ifndef IPC_SHM_H
#define IPC_SHM_H

#include "../types.h"

// Maximum number of shared memory regions
#define SHM_MAX_REGIONS     64

// Maximum size of a single shared memory region (16MB)
#define SHM_MAX_SIZE        (16 * 1024 * 1024)

// Shared memory flags
#define SHM_FLAG_NONE       0x00
#define SHM_FLAG_READONLY   0x01    // Read-only for non-creators
#define SHM_FLAG_EXCLUSIVE  0x02    // Only one process can map at a time

// Shared memory region state
typedef enum {
    SHM_STATE_FREE = 0,     // Region is available
    SHM_STATE_ALLOCATED,    // Region is allocated but not yet mapped
    SHM_STATE_MAPPED,       // Region is mapped by at least one process
} shm_state_t;

// Mapping entry - tracks who has mapped a region
typedef struct shm_mapping {
    uint32_t pid;               // Process ID that has this mapping
    uint64_t virt_addr;         // Virtual address in that process
    struct shm_mapping *next;   // Next mapping in list
} shm_mapping_t;

// Shared memory region descriptor
typedef struct {
    int id;                     // Region ID (index in array)
    shm_state_t state;          // Current state
    uint32_t creator_pid;       // Process that created this region
    uint64_t phys_addr;         // Physical address of shared memory
    size_t size;                // Size in bytes (page-aligned)
    uint32_t flags;             // Creation flags
    uint32_t ref_count;         // Number of active mappings
    shm_mapping_t *mappings;    // List of current mappings
} shm_region_t;

// ============================================================================
// Shared Memory API
// ============================================================================

/**
 * Initialize the shared memory subsystem
 * Called once during kernel startup
 */
void shm_init(void);

/**
 * Create a new shared memory region
 * @param size      Size in bytes (will be page-aligned)
 * @param flags     SHM_FLAG_* flags
 * @return          Shared memory ID (>= 0), or -1 on failure
 */
int64_t sys_shm_create(size_t size, int flags);

/**
 * Map a shared memory region into the calling process's address space
 * @param id        Shared memory ID from sys_shm_create
 * @param addr      Output: virtual address where memory was mapped
 * @return          0 on success, -1 on failure
 */
int64_t sys_shm_map(int id, void **addr);

/**
 * Unmap a shared memory region from the calling process
 * @param id        Shared memory ID
 * @return          0 on success, -1 on failure
 */
int64_t sys_shm_unmap(int id);

/**
 * Destroy a shared memory region (creator only)
 * All mappings are invalidated, physical memory is freed
 * @param id        Shared memory ID
 * @return          0 on success, -1 on failure
 */
int64_t sys_shm_destroy(int id);

/**
 * Get information about a shared memory region
 * @param id        Shared memory ID
 * @param size      Output: size of the region (optional, can be NULL)
 * @param ref_count Output: number of current mappings (optional)
 * @return          0 on success, -1 on failure
 */
int64_t sys_shm_info(int id, size_t *size, uint32_t *ref_count);

/**
 * Clean up all shared memory for a process that is exiting
 * Called automatically when a process terminates
 * @param pid       Process ID being cleaned up
 */
void shm_cleanup_process(uint32_t pid);

#endif // IPC_SHM_H
