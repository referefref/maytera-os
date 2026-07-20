// virtio.h - VirtIO Framework Main Header
// MayteraOS Production VirtIO Driver System
//
// This unified header provides access to all VirtIO drivers:
// - virtio_core: Core transport and virtqueue management
// - virtio_blk: Block storage devices
// - virtio_console: Serial console
// - virtio_gpu: Graphics
// - virtio_input: Keyboard/mouse
//
// Existing virtio_net driver in net/ is enhanced by this framework

#ifndef VIRTIO_H
#define VIRTIO_H

#include "virtio_core.h"
#include "virtio_blk.h"
#include "virtio_console.h"
#include "virtio_gpu.h"
#include "virtio_input.h"

// ============================================================================
// VirtIO Subsystem Initialization
// ============================================================================

// Initialize entire VirtIO subsystem
// Discovers all VirtIO devices and initializes available drivers
int virtio_subsystem_init(void);

// Print status of all VirtIO devices
void virtio_print_status(void);

// Get number of VirtIO devices
int virtio_get_device_count(void);

// Check if specific device type is available
bool virtio_has_device(virtio_device_type_t type);

// ============================================================================
// Legacy Compatibility
// ============================================================================

// The existing VirtIO network driver in net/virtio_net.c
// continues to work independently. This framework provides
// a more comprehensive implementation that can be migrated to.

#endif // VIRTIO_H
