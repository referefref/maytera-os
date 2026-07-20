// usb_msc.c - USB Mass Storage Class Driver (Enhanced)
#include "usb_msc.h"
#include "../serial.h"
#include "../string.h"
#include "../mm/heap.h"
#include "../mm/pmm.h"
#include "../fs/bootlog.h"

// =============================================================================
// Global State
// =============================================================================

#define MAX_MSC_DEVICES 8
static usb_msc_device_t msc_devices[MAX_MSC_DEVICES];
static int msc_device_count = 0;
static usb_msc_hotplug_callback_t hotplug_callback = NULL;

// Byte swap helpers for big-endian SCSI data
static inline uint16_t be16_to_cpu(uint16_t val) {
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

static inline uint32_t be32_to_cpu(uint32_t val) {
    return ((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val & 0xFF0000) >> 8) |
           ((val & 0xFF000000) >> 24);
}

static inline uint64_t be64_to_cpu(uint64_t val) {
    return ((uint64_t)be32_to_cpu(val & 0xFFFFFFFF) << 32) |
           be32_to_cpu(val >> 32);
}

static inline uint32_t cpu_to_be32(uint32_t val) {
    return be32_to_cpu(val);
}

// Fire hotplug event
static void fire_hotplug_event(usb_msc_event_type_t type, int device_index) {
    if (hotplug_callback) {
        usb_msc_event_t event;
        event.type = type;
        event.device_index = device_index;
        event.device = (device_index >= 0 && device_index < msc_device_count) ?
                       &msc_devices[device_index] : NULL;
        hotplug_callback(&event);
    }
}

// =============================================================================
// Initialization
// =============================================================================

void usb_msc_init(void) {
    kprintf("[USB-MSC] Initializing Mass Storage subsystem...\n");
    msc_device_count = 0;
    memset(msc_devices, 0, sizeof(msc_devices));
    hotplug_callback = NULL;
}

// =============================================================================
// Hotplug Callback Registration
// =============================================================================

void usb_msc_register_hotplug_callback(usb_msc_hotplug_callback_t callback) {
    hotplug_callback = callback;
}

void usb_msc_unregister_hotplug_callback(void) {
    hotplug_callback = NULL;
}

// =============================================================================
// Mass Storage Class Requests
// =============================================================================

int usb_msc_reset(usb_msc_device_t *dev) {
    if (!dev || !dev->controller) return -1;

    kprintf("[USB-MSC] Performing bulk-only mass storage reset (slot %d)\n", dev->slot_id);

    int result = xhci_control_transfer(dev->controller, dev->slot_id,
        0x21,                   // bmRequestType: Host to Device, Class, Interface
        USB_MSC_REQ_RESET,      // bRequest
        0,                      // wValue
        dev->interface_num,     // wIndex
        NULL, 0);

    if (result < 0) {
        kprintf("[USB-MSC] Reset failed\n");
    }

    return result;
}

int usb_msc_get_max_lun(usb_msc_device_t *dev) {
    if (!dev || !dev->controller) return -1;

    uint8_t max_lun = 0;
    int result = xhci_control_transfer(dev->controller, dev->slot_id,
        0xA1,                   // bmRequestType: Device to Host, Class, Interface
        USB_MSC_REQ_GET_MAX_LUN,// bRequest
        0,                      // wValue
        dev->interface_num,     // wIndex
        &max_lun, 1);

    if (result >= 0) {
        dev->max_lun = max_lun;
        kprintf("[USB-MSC] Max LUN: %d\n", max_lun);
        return max_lun;
    }

    // Device may STALL if it only has one LUN - this is not an error
    dev->max_lun = 0;
    return 0;
}

// =============================================================================
// Endpoint Stall Handling
// =============================================================================

int usb_msc_clear_stall(usb_msc_device_t *dev, int endpoint) {
    if (!dev || !dev->controller) return -1;

    uint8_t ep_addr = endpoint | (endpoint == dev->bulk_in_ep ? 0x80 : 0x00);

    return xhci_control_transfer(dev->controller, dev->slot_id,
        0x02,                   // bmRequestType: Host to Device, Standard, Endpoint
        0x01,                   // bRequest: CLEAR_FEATURE
        0x00,                   // wValue: ENDPOINT_HALT
        ep_addr,                // wIndex: endpoint address
        NULL, 0);
}

int usb_msc_bulk_reset_recovery(usb_msc_device_t *dev) {
    if (!dev) return -1;

    kprintf("[USB-MSC] Performing bulk reset recovery\n");

    // 1. Bulk-Only Mass Storage Reset
    usb_msc_reset(dev);

    // 2. Clear HALT on Bulk-In endpoint
    usb_msc_clear_stall(dev, dev->bulk_in_ep);

    // 3. Clear HALT on Bulk-Out endpoint
    usb_msc_clear_stall(dev, dev->bulk_out_ep);

    return 0;
}

// =============================================================================
// Bulk-Only Transport
// =============================================================================

// #307: A USB Mass Storage command is a three-phase Bulk-Only Transport
// sequence (CBW out, optional data, CSW in) that must run atomically on the
// device. Once the scheduler is up, many threads read files concurrently
// (config self-tests, services, apps) and all funnel through blk_read ->
// usb_msc_read -> usb_msc_transport. Without serialization their CBW/data/CSW
// phases interleave on the same bulk endpoints, the device sees a protocol
// violation and STALLs, and the whole stack wedges ("Invalid CSW signature",
// endless reset recovery). This lock forces one full command at a time. It is
// a yielding lock so a waiter gives the CPU to the in-flight command's thread
// (busy-spinning here would starve the holder under the cooperative parts of
// the scheduler). Uncontended during early single-threaded boot.
static volatile int g_msc_cmd_busy = 0;
static void msc_cmd_lock(void) {
    extern void proc_yield(void);
    extern void proc_sleep(uint32_t ms);
    extern _Bool sched_preemption_enabled(void);
    while (__sync_lock_test_and_set(&g_msc_cmd_busy, 1)) {
        // #375: at runtime the lock holder SLEEPS during its (slow-USB) transfer
        // wait, so a waiter that merely proc_yield()s here would re-queue itself
        // READY and busy-spin through the scheduler, pegging a core. Sleep
        // instead so the core can idle-HLT while the in-flight command runs.
        // Early boot (no preemption) keeps the cooperative yield.
        if (sched_preemption_enabled()) proc_sleep(1);
        else proc_yield();
    }
}
static void msc_cmd_unlock(void) {
    __sync_lock_release(&g_msc_cmd_busy);
}

static int usb_msc_transport_inner(usb_msc_device_t *dev, usb_msc_cbw_t *cbw,
                                   void *data, usb_msc_csw_t *csw);

int usb_msc_transport(usb_msc_device_t *dev, usb_msc_cbw_t *cbw,
                      void *data, usb_msc_csw_t *csw) {
    msc_cmd_lock();
    int r = usb_msc_transport_inner(dev, cbw, data, csw);
    msc_cmd_unlock();
    return r;
}

static int usb_msc_transport_inner(usb_msc_device_t *dev, usb_msc_cbw_t *cbw,
                                   void *data, usb_msc_csw_t *csw) {
    if (!dev || !dev->controller || !cbw || !csw) return -1;

    int retry_count = 0;
    const int max_retries = 3;

retry:
    // Send CBW on Bulk OUT endpoint
    int result = xhci_bulk_transfer(dev->controller, dev->slot_id,
                                    dev->bulk_out_ep, cbw, USB_MSC_CBW_SIZE, 0);
    if (result < 0) {
        kprintf("[USB-MSC] Failed to send CBW (result=%d)\n", result);
        bootlog_write("[USB-MSC] slot %d: send CBW FAILED (result=%d) attempt %d/%d",
                      dev->slot_id, result, retry_count + 1, max_retries);
        if (retry_count++ < max_retries) {
            usb_msc_bulk_reset_recovery(dev);
            goto retry;
        }
        bootlog_write("[USB-MSC] slot %d: giving up after %d CBW retries", dev->slot_id, max_retries);
        return -1;
    }

    // Data phase (if any)
    if (cbw->data_transfer_len > 0 && data) {
        int direction = (cbw->flags & 0x80) ? 1 : 0;  // IN or OUT
        int ep = direction ? dev->bulk_in_ep : dev->bulk_out_ep;

        result = xhci_bulk_transfer(dev->controller, dev->slot_id,
                                    ep, data, cbw->data_transfer_len, direction);
        if (result < 0) {
            kprintf("[USB-MSC] Data transfer failed (ep=%d, dir=%d, len=%d)\n",
                    ep, direction, cbw->data_transfer_len);
            bootlog_write("[USB-MSC] slot %d: data transfer FAILED ep=%d dir=%d len=%d",
                          dev->slot_id, ep, direction, cbw->data_transfer_len);
            // Try to get CSW anyway
        }
    }

    // Receive CSW on Bulk IN endpoint
    memset(csw, 0, sizeof(usb_msc_csw_t));
    result = xhci_bulk_transfer(dev->controller, dev->slot_id,
                                dev->bulk_in_ep, csw, USB_MSC_CSW_SIZE, 1);
    if (result < 0) {
        kprintf("[USB-MSC] Failed to receive CSW\n");
        bootlog_write("[USB-MSC] slot %d: receive CSW FAILED, clearing stall and retrying once", dev->slot_id);
        // Try clear stall and get CSW again
        usb_msc_clear_stall(dev, dev->bulk_in_ep);
        result = xhci_bulk_transfer(dev->controller, dev->slot_id,
                                    dev->bulk_in_ep, csw, USB_MSC_CSW_SIZE, 1);
        if (result < 0) {
            bootlog_write("[USB-MSC] slot %d: receive CSW FAILED again, attempt %d/%d",
                          dev->slot_id, retry_count + 1, max_retries);
            if (retry_count++ < max_retries) {
                usb_msc_bulk_reset_recovery(dev);
                goto retry;
            }
            bootlog_write("[USB-MSC] slot %d: giving up after %d CSW retries", dev->slot_id, max_retries);
            return -1;
        }
    }

    // Validate CSW
    if (csw->signature != USB_MSC_CSW_SIGNATURE) {
        kprintf("[USB-MSC] Invalid CSW signature: 0x%08x\n", csw->signature);
        bootlog_write("[USB-MSC] slot %d: invalid CSW signature 0x%08x, attempt %d/%d",
                      dev->slot_id, csw->signature, retry_count + 1, max_retries);
        if (retry_count++ < max_retries) {
            usb_msc_bulk_reset_recovery(dev);
            goto retry;
        }
        return -1;
    }

    if (csw->tag != cbw->tag) {
        kprintf("[USB-MSC] CSW tag mismatch: expected %d, got %d\n", cbw->tag, csw->tag);
        return -1;
    }

    if (csw->status == USB_MSC_CSW_PHASE_ERROR) {
        kprintf("[USB-MSC] Phase error - performing recovery\n");
        usb_msc_bulk_reset_recovery(dev);
        return -1;
    }

    if (csw->status != USB_MSC_CSW_PASSED) {
        // Command failed but transport succeeded - not a transport error
        return -csw->status;
    }

    return 0;
}

// =============================================================================
// SCSI Commands
// =============================================================================

int usb_msc_test_unit_ready(usb_msc_device_t *dev, uint8_t lun) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = 0;
    cbw.flags = 0;
    cbw.lun = lun;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_TEST_UNIT_READY;

    return usb_msc_transport(dev, &cbw, NULL, &csw);
}

int usb_msc_inquiry(usb_msc_device_t *dev, uint8_t lun, scsi_inquiry_t *inquiry) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = sizeof(scsi_inquiry_t);
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_INQUIRY;
    cbw.cb[4] = sizeof(scsi_inquiry_t);

    memset(inquiry, 0, sizeof(scsi_inquiry_t));
    return usb_msc_transport(dev, &cbw, inquiry, &csw);
}

int usb_msc_read_capacity(usb_msc_device_t *dev, uint8_t lun) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;
    scsi_read_capacity_10_t cap;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = sizeof(cap);
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_READ_CAPACITY_10;

    memset(&cap, 0, sizeof(cap));
    int result = usb_msc_transport(dev, &cbw, &cap, &csw);

    if (result >= 0) {
        uint32_t last_lba = be32_to_cpu(cap.last_lba);
        uint32_t block_size = be32_to_cpu(cap.block_size);

        if (lun < USB_MSC_MAX_LUNS) {
            dev->luns[lun].num_blocks = (uint64_t)last_lba + 1;
            dev->luns[lun].block_size = block_size;
        }

        // Also update main device info for LUN 0
        if (lun == 0) {
            dev->num_blocks = (uint64_t)last_lba + 1;
            dev->block_size = block_size;
        }

        uint64_t capacity_mb = ((uint64_t)last_lba + 1) * block_size / (1024 * 1024);
        kprintf("[USB-MSC] LUN %d: Capacity %llu blocks x %u bytes = %llu MB\n",
                lun, (uint64_t)last_lba + 1, block_size, capacity_mb);
        bootlog_write("[USB-MSC] LUN %d: capacity %llu blocks x %u bytes = %llu MB",
                      lun, (unsigned long long)last_lba + 1, block_size,
                      (unsigned long long)capacity_mb);

        // If last_lba is 0xFFFFFFFF, need to use READ CAPACITY (16)
        if (last_lba == 0xFFFFFFFF) {
            kprintf("[USB-MSC] LUN %d: Drive >2TB, using READ CAPACITY (16)\n", lun);
            return usb_msc_read_capacity_16(dev, lun);
        }
    }

    return result;
}

int usb_msc_read_capacity_16(usb_msc_device_t *dev, uint8_t lun) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;
    scsi_read_capacity_16_t cap;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = sizeof(cap);
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 16;
    cbw.cb[0] = SCSI_SERVICE_ACTION_IN;
    cbw.cb[1] = 0x10;  // READ CAPACITY (16)
    cbw.cb[13] = sizeof(cap);  // Allocation length

    memset(&cap, 0, sizeof(cap));
    int result = usb_msc_transport(dev, &cbw, &cap, &csw);

    if (result >= 0) {
        uint64_t last_lba = be64_to_cpu(cap.last_lba);
        uint32_t block_size = be32_to_cpu(cap.block_size);

        if (lun < USB_MSC_MAX_LUNS) {
            dev->luns[lun].num_blocks = last_lba + 1;
            dev->luns[lun].block_size = block_size;
        }

        if (lun == 0) {
            dev->num_blocks = last_lba + 1;
            dev->block_size = block_size;
        }

        uint64_t capacity_gb = (last_lba + 1) * block_size / (1024 * 1024 * 1024);
        kprintf("[USB-MSC] LUN %d: Capacity %llu blocks x %u bytes = %llu GB\n",
                lun, last_lba + 1, block_size, capacity_gb);
    }

    return result;
}

int usb_msc_request_sense(usb_msc_device_t *dev, uint8_t lun, scsi_sense_t *sense) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = sizeof(scsi_sense_t);
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_REQUEST_SENSE;
    cbw.cb[4] = sizeof(scsi_sense_t);

    memset(sense, 0, sizeof(scsi_sense_t));
    return usb_msc_transport(dev, &cbw, sense, &csw);
}

int usb_msc_start_stop_unit(usb_msc_device_t *dev, uint8_t lun, int start, int load_eject) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = 0;
    cbw.flags = 0;
    cbw.lun = lun;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_START_STOP_UNIT;
    cbw.cb[4] = (load_eject ? 0x02 : 0x00) | (start ? 0x01 : 0x00);

    return usb_msc_transport(dev, &cbw, NULL, &csw);
}

int usb_msc_prevent_allow_removal(usb_msc_device_t *dev, uint8_t lun, int prevent) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = 0;
    cbw.flags = 0;
    cbw.lun = lun;
    cbw.cb_length = 6;
    cbw.cb[0] = SCSI_PREVENT_ALLOW_MEDIUM_REMOVAL;
    cbw.cb[4] = prevent ? 0x01 : 0x00;

    return usb_msc_transport(dev, &cbw, NULL, &csw);
}

// =============================================================================
// Block I/O
// =============================================================================

int usb_msc_read(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                 void *buf, uint32_t num_blocks) {
    if (!dev || !buf) return -1;

    usb_msc_lun_t *lunp = (lun < USB_MSC_MAX_LUNS) ? &dev->luns[lun] : NULL;
    uint64_t max_blocks = lunp ? lunp->num_blocks : dev->num_blocks;
    uint32_t block_size = lunp ? lunp->block_size : dev->block_size;

    if (lba + num_blocks > max_blocks) {
        kprintf("[USB-MSC] Read beyond end of device: lba=%llu + count=%u > max=%llu\n",
                lba, num_blocks, max_blocks);
        return -1;
    }

    // Use READ(16) for large LBAs
    if (lba > 0xFFFFFFFF || num_blocks > 0xFFFF) {
        return usb_msc_read_16(dev, lun, lba, buf, num_blocks);
    }

    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = num_blocks * block_size;
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_READ_10;
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (num_blocks >> 8) & 0xFF;
    cbw.cb[8] = num_blocks & 0xFF;

    return usb_msc_transport(dev, &cbw, buf, &csw);
}

int usb_msc_write(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                  const void *buf, uint32_t num_blocks) {
    if (!dev || !buf) return -1;

    usb_msc_lun_t *lunp = (lun < USB_MSC_MAX_LUNS) ? &dev->luns[lun] : NULL;
    uint64_t max_blocks = lunp ? lunp->num_blocks : dev->num_blocks;
    uint32_t block_size = lunp ? lunp->block_size : dev->block_size;

    if (lba + num_blocks > max_blocks) return -1;

    // Check write protection
    if (lunp && lunp->write_protected) {
        kprintf("[USB-MSC] Write to write-protected media\n");
        return -1;
    }

    // Use WRITE(16) for large LBAs
    if (lba > 0xFFFFFFFF || num_blocks > 0xFFFF) {
        return usb_msc_write_16(dev, lun, lba, buf, num_blocks);
    }

    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = num_blocks * block_size;
    cbw.flags = 0x00;  // Data OUT
    cbw.lun = lun;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_WRITE_10;
    cbw.cb[2] = (lba >> 24) & 0xFF;
    cbw.cb[3] = (lba >> 16) & 0xFF;
    cbw.cb[4] = (lba >> 8) & 0xFF;
    cbw.cb[5] = lba & 0xFF;
    cbw.cb[7] = (num_blocks >> 8) & 0xFF;
    cbw.cb[8] = num_blocks & 0xFF;

    return usb_msc_transport(dev, &cbw, (void *)buf, &csw);
}

int usb_msc_read_16(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                    void *buf, uint32_t num_blocks) {
    if (!dev || !buf) return -1;

    usb_msc_lun_t *lunp = (lun < USB_MSC_MAX_LUNS) ? &dev->luns[lun] : NULL;
    uint32_t block_size = lunp ? lunp->block_size : dev->block_size;

    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = num_blocks * block_size;
    cbw.flags = 0x80;  // Data IN
    cbw.lun = lun;
    cbw.cb_length = 16;
    cbw.cb[0] = SCSI_READ_16;
    cbw.cb[2] = (lba >> 56) & 0xFF;
    cbw.cb[3] = (lba >> 48) & 0xFF;
    cbw.cb[4] = (lba >> 40) & 0xFF;
    cbw.cb[5] = (lba >> 32) & 0xFF;
    cbw.cb[6] = (lba >> 24) & 0xFF;
    cbw.cb[7] = (lba >> 16) & 0xFF;
    cbw.cb[8] = (lba >> 8) & 0xFF;
    cbw.cb[9] = lba & 0xFF;
    cbw.cb[10] = (num_blocks >> 24) & 0xFF;
    cbw.cb[11] = (num_blocks >> 16) & 0xFF;
    cbw.cb[12] = (num_blocks >> 8) & 0xFF;
    cbw.cb[13] = num_blocks & 0xFF;

    return usb_msc_transport(dev, &cbw, buf, &csw);
}

int usb_msc_write_16(usb_msc_device_t *dev, uint8_t lun, uint64_t lba,
                     const void *buf, uint32_t num_blocks) {
    if (!dev || !buf) return -1;

    usb_msc_lun_t *lunp = (lun < USB_MSC_MAX_LUNS) ? &dev->luns[lun] : NULL;
    uint32_t block_size = lunp ? lunp->block_size : dev->block_size;

    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = num_blocks * block_size;
    cbw.flags = 0x00;  // Data OUT
    cbw.lun = lun;
    cbw.cb_length = 16;
    cbw.cb[0] = SCSI_WRITE_16;
    cbw.cb[2] = (lba >> 56) & 0xFF;
    cbw.cb[3] = (lba >> 48) & 0xFF;
    cbw.cb[4] = (lba >> 40) & 0xFF;
    cbw.cb[5] = (lba >> 32) & 0xFF;
    cbw.cb[6] = (lba >> 24) & 0xFF;
    cbw.cb[7] = (lba >> 16) & 0xFF;
    cbw.cb[8] = (lba >> 8) & 0xFF;
    cbw.cb[9] = lba & 0xFF;
    cbw.cb[10] = (num_blocks >> 24) & 0xFF;
    cbw.cb[11] = (num_blocks >> 16) & 0xFF;
    cbw.cb[12] = (num_blocks >> 8) & 0xFF;
    cbw.cb[13] = num_blocks & 0xFF;

    return usb_msc_transport(dev, &cbw, (void *)buf, &csw);
}

int usb_msc_sync(usb_msc_device_t *dev, uint8_t lun) {
    usb_msc_cbw_t cbw;
    usb_msc_csw_t csw;

    memset(&cbw, 0, sizeof(cbw));
    cbw.signature = USB_MSC_CBW_SIGNATURE;
    cbw.tag = ++dev->tag;
    cbw.data_transfer_len = 0;
    cbw.flags = 0;
    cbw.lun = lun;
    cbw.cb_length = 10;
    cbw.cb[0] = SCSI_SYNCHRONIZE_CACHE;

    return usb_msc_transport(dev, &cbw, NULL, &csw);
}

// =============================================================================
// Device Enumeration
// =============================================================================

// Helper to trim trailing spaces from SCSI strings
static void trim_scsi_string(char *dest, const char *src, int len) {
    memcpy(dest, src, len);
    dest[len] = '\0';
    // Trim trailing spaces
    for (int i = len - 1; i >= 0 && dest[i] == ' '; i--) {
        dest[i] = '\0';
    }
}

// Probe and enumerate a single LUN
static int usb_msc_probe_lun(usb_msc_device_t *dev, uint8_t lun) {
    if (lun >= USB_MSC_MAX_LUNS) return -1;

    usb_msc_lun_t *lunp = &dev->luns[lun];
    memset(lunp, 0, sizeof(usb_msc_lun_t));

    // Test unit ready (may fail initially if media not present)
    int ready_retries = 5;
    while (ready_retries > 0) {
        if (usb_msc_test_unit_ready(dev, lun) >= 0) {
            lunp->ready = 1;
            break;
        }
        ready_retries--;

        // Get sense data to clear error and check reason
        scsi_sense_t sense;
        if (usb_msc_request_sense(dev, lun, &sense) >= 0) {
            uint8_t sense_key = sense.sense_key & 0x0F;
            if (sense_key == SCSI_SENSE_UNIT_ATTENTION) {
                // Unit attention - media may have changed, retry
                continue;
            } else if (sense_key == SCSI_SENSE_NOT_READY) {
                // Not ready - media not present or initializing
                kprintf("[USB-MSC] LUN %d: Not ready (ASC=%02x ASCQ=%02x)\n",
                        lun, sense.asc, sense.ascq);
                bootlog_write("[USB-MSC] LUN %d: NOT READY (ASC=%02x ASCQ=%02x)",
                              lun, sense.asc, sense.ascq);
                break;
            }
        }
    }
    if (!lunp->ready) {
        bootlog_write("[USB-MSC] LUN %d: TEST UNIT READY failed after retries", lun);
    }

    // Inquiry - always succeeds even if not ready
    scsi_inquiry_t inquiry;
    int inq_result = usb_msc_inquiry(dev, lun, &inquiry);
    if (inq_result >= 0) {
        trim_scsi_string(lunp->vendor, inquiry.vendor, 8);
        trim_scsi_string(lunp->product, inquiry.product, 16);
        trim_scsi_string(lunp->revision, inquiry.revision, 4);
        lunp->removable = (inquiry.removable & 0x80) ? 1 : 0;

        kprintf("[USB-MSC] LUN %d: %s %s (rev %s)\n",
                lun, lunp->vendor, lunp->product, lunp->revision);
        bootlog_write("[USB-MSC] LUN %d INQUIRY: vendor='%s' product='%s' rev='%s' removable=%d ready=%d",
                      lun, lunp->vendor, lunp->product, lunp->revision,
                      lunp->removable, lunp->ready);

        if (lunp->removable) {
            kprintf("[USB-MSC] LUN %d: Removable media\n", lun);
        }
    } else {
        bootlog_write("[USB-MSC] LUN %d: INQUIRY FAILED (result=%d)", lun, inq_result);
    }

    // Read capacity if ready
    if (lunp->ready) {
        usb_msc_read_capacity(dev, lun);
    }

    return 0;
}

int usb_msc_enumerate(xhci_controller_t *xhc, int slot_id, int interface_num,
                      int bulk_in_ep, int bulk_out_ep,
                      int max_packet_in, int max_packet_out) {
    if (msc_device_count >= MAX_MSC_DEVICES) {
        kprintf("[USB-MSC] Maximum devices reached\n");
        return -1;
    }

    usb_msc_device_t *dev = &msc_devices[msc_device_count];
    memset(dev, 0, sizeof(usb_msc_device_t));

    dev->controller = xhc;
    dev->slot_id = slot_id;
    dev->interface_num = interface_num;
    dev->bulk_in_ep = bulk_in_ep;
    dev->bulk_out_ep = bulk_out_ep;
    dev->max_packet_in = max_packet_in;
    dev->max_packet_out = max_packet_out;
    dev->block_size = 512;  // Default

    kprintf("[USB-MSC] Enumerating device (slot %d, IN EP %d, OUT EP %d)\n",
            slot_id, bulk_in_ep, bulk_out_ep);
    bootlog_write("[USB-MSC] Enumerating slot %d: bulk IN ep %d (mps %d), bulk OUT ep %d (mps %d)",
                  slot_id, bulk_in_ep, max_packet_in, bulk_out_ep, max_packet_out);

    // Get max LUN
    usb_msc_get_max_lun(dev);
    bootlog_write("[USB-MSC] slot %d: max LUN %d", slot_id, dev->max_lun);

    // Probe each LUN
    for (int lun = 0; lun <= dev->max_lun; lun++) {
        usb_msc_probe_lun(dev, lun);
    }

    // Copy LUN 0 info to main device struct for compatibility
    usb_msc_lun_t *lun0 = &dev->luns[0];
    memcpy(dev->vendor, lun0->vendor, sizeof(dev->vendor));
    memcpy(dev->product, lun0->product, sizeof(dev->product));
    dev->num_blocks = lun0->num_blocks;
    dev->block_size = lun0->block_size;
    dev->ready = lun0->ready;
    dev->removable = lun0->removable;

    int device_index = msc_device_count;
    msc_device_count++;

    // Fire hotplug event
    fire_hotplug_event(USB_MSC_EVENT_INSERTED, device_index);

    if (dev->ready) {
        fire_hotplug_event(USB_MSC_EVENT_MOUNT_READY, device_index);
    }

    return device_index;
}

int usb_msc_probe(xhci_controller_t *xhc, int slot_id,
                  uint8_t interface_class, uint8_t interface_subclass,
                  uint8_t interface_protocol) {
    // Check if this is a Mass Storage device
    if (interface_class != USB_MSC_CLASS) {
        return -1;
    }

    // Only support BBB transport with SCSI command set
    if (interface_subclass != USB_MSC_SUBCLASS_SCSI ||
        interface_protocol != USB_MSC_PROTOCOL_BBB) {
        kprintf("[USB-MSC] Unsupported subclass/protocol: %02x/%02x\n",
                interface_subclass, interface_protocol);
        return -1;
    }

    kprintf("[USB-MSC] Detected USB Mass Storage device (slot %d)\n", slot_id);

    // Use default endpoint numbers - caller should provide actual values
    // from configuration descriptor parsing
    return usb_msc_enumerate(xhc, slot_id, 0, 1, 2, 512, 512);
}

// =============================================================================
// Device Access and Management
// =============================================================================

usb_msc_device_t *usb_msc_get_device(int index) {
    if (index < 0 || index >= msc_device_count) {
        return NULL;
    }
    return &msc_devices[index];
}

int usb_msc_get_device_count(void) {
    return msc_device_count;
}

int usb_msc_find_device_by_slot(int slot_id) {
    for (int i = 0; i < msc_device_count; i++) {
        if (msc_devices[i].slot_id == slot_id) {
            return i;
        }
    }
    return -1;
}

void usb_msc_device_removed(int slot_id) {
    int index = usb_msc_find_device_by_slot(slot_id);
    if (index < 0) return;

    kprintf("[USB-MSC] Device removed (slot %d)\n", slot_id);

    // Fire removal event
    fire_hotplug_event(USB_MSC_EVENT_REMOVED, index);

    // Remove device from list
    for (int i = index; i < msc_device_count - 1; i++) {
        msc_devices[i] = msc_devices[i + 1];
    }
    msc_device_count--;
}

// =============================================================================
// Safe Eject
// =============================================================================

int usb_msc_eject(usb_msc_device_t *dev) {
    if (!dev) return -1;

    kprintf("[USB-MSC] Ejecting device %s %s\n", dev->vendor, dev->product);

    // Sync all LUNs
    for (int lun = 0; lun <= dev->max_lun; lun++) {
        usb_msc_sync(dev, lun);
    }

    // Allow removal
    for (int lun = 0; lun <= dev->max_lun; lun++) {
        usb_msc_prevent_allow_removal(dev, lun, 0);
    }

    // For removable media, eject
    if (dev->removable) {
        for (int lun = 0; lun <= dev->max_lun; lun++) {
            usb_msc_start_stop_unit(dev, lun, 0, 1);  // Stop and eject
        }
    }

    dev->mounted = 0;

    return 0;
}

int usb_msc_safe_remove(int device_index) {
    usb_msc_device_t *dev = usb_msc_get_device(device_index);
    if (!dev) return -1;

    int result = usb_msc_eject(dev);

    fire_hotplug_event(USB_MSC_EVENT_UNMOUNTED, device_index);

    return result;
}

// =============================================================================
// Hotplug Polling
// =============================================================================

void usb_msc_poll_hotplug(void) {
    // This would be called periodically to check for:
    // - Media changes on removable drives
    // - Device state changes
    // For now, hotplug is handled by xHCI port change events

    // Check for media ready state changes on removable devices
    for (int i = 0; i < msc_device_count; i++) {
        usb_msc_device_t *dev = &msc_devices[i];
        if (!dev->removable) continue;

        // Test unit ready on LUN 0
        int was_ready = dev->luns[0].ready;
        int result = usb_msc_test_unit_ready(dev, 0);
        int is_ready = (result >= 0);

        if (is_ready && !was_ready) {
            // Media inserted
            dev->luns[0].ready = 1;
            usb_msc_read_capacity(dev, 0);
            fire_hotplug_event(USB_MSC_EVENT_MEDIA_CHANGE, i);
        } else if (!is_ready && was_ready) {
            // Media removed
            dev->luns[0].ready = 0;
            fire_hotplug_event(USB_MSC_EVENT_MEDIA_CHANGE, i);
        }
    }
}

// =============================================================================
// Debug
// =============================================================================

const char *usb_msc_sense_key_name(uint8_t sense_key) {
    switch (sense_key & 0x0F) {
        case SCSI_SENSE_NO_SENSE:       return "No Sense";
        case SCSI_SENSE_RECOVERED:      return "Recovered Error";
        case SCSI_SENSE_NOT_READY:      return "Not Ready";
        case SCSI_SENSE_MEDIUM_ERROR:   return "Medium Error";
        case SCSI_SENSE_HARDWARE_ERROR: return "Hardware Error";
        case SCSI_SENSE_ILLEGAL_REQUEST: return "Illegal Request";
        case SCSI_SENSE_UNIT_ATTENTION: return "Unit Attention";
        case SCSI_SENSE_DATA_PROTECT:   return "Data Protect";
        case SCSI_SENSE_BLANK_CHECK:    return "Blank Check";
        case SCSI_SENSE_ABORTED_COMMAND: return "Aborted Command";
        default: return "Unknown";
    }
}

void usb_msc_print_devices(void) {
    kprintf("\n[USB-MSC] Device List (%d devices):\n", msc_device_count);
    for (int i = 0; i < msc_device_count; i++) {
        usb_msc_device_t *dev = &msc_devices[i];
        kprintf("  %d: %s %s (slot %d)\n", i, dev->vendor, dev->product, dev->slot_id);

        for (int lun = 0; lun <= dev->max_lun; lun++) {
            usb_msc_lun_t *lunp = &dev->luns[lun];
            if (lunp->ready) {
                uint64_t capacity_mb = (lunp->num_blocks * lunp->block_size) / (1024 * 1024);
                kprintf("     LUN %d: %llu MB, block size %u\n",
                        lun, capacity_mb, lunp->block_size);
            } else {
                kprintf("     LUN %d: Not ready\n", lun);
            }
        }
    }
    if (msc_device_count == 0) {
        kprintf("  (no devices)\n");
    }
}

// =============================================================================
// #307: Mass Storage boot self-test. Proves the bulk/SCSI path end to end by
// issuing a SCSI READ(10) of the first sectors and listing the root directory
// of a FAT12/16/32 filesystem found on the USB stick. Serial evidence for the
// live-USB bring-up on the iMac (#307).
// =============================================================================

extern int  proc_create(const char *name, void (*entry)(void *), void *arg, int prio);
extern void proc_sleep(uint32_t ms);

static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void msc_list_dir_sector(const uint8_t *sec, uint32_t bytes, int *listed) {
    for (uint32_t off = 0; off + 32 <= bytes; off += 32) {
        const uint8_t *e = sec + off;
        if (e[0] == 0x00) return;             // end of directory
        if (e[0] == 0xE5) continue;           // deleted
        if ((e[11] & 0x0F) == 0x0F) continue; // LFN component
        if (e[11] & 0x08) {                   // volume label
            char label[12]; memcpy(label, e, 11); label[11] = 0;
            kprintf("[USB-MSC]   volume label: '%s'\n", label);
            continue;
        }
        char name[13];
        int n = 0;
        for (int i = 0; i < 8 && e[i] != ' '; i++) name[n++] = e[i];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (int i = 8; i < 11 && e[i] != ' '; i++) name[n++] = e[i];
        }
        name[n] = 0;
        uint32_t size = rd32(e + 28);
        if (e[11] & 0x10)
            kprintf("[USB-MSC]   %s <DIR>\n", name);
        else
            kprintf("[USB-MSC]   %s (%u bytes)\n", name, size);
        (*listed)++;
    }
}

void usb_msc_selftest(void) {
    if (msc_device_count == 0) {
        kprintf("[USB-MSC] self-test: no mass-storage device present\n");
        return;
    }
    usb_msc_device_t *dev = usb_msc_get_device(0);
    if (!dev || !dev->luns[0].ready) {
        kprintf("[USB-MSC] self-test: LUN 0 not ready\n");
        return;
    }
    uint32_t bs = dev->luns[0].block_size ? dev->luns[0].block_size : 512;
    uint8_t *buf = (uint8_t *)pmm_alloc_pages(2);   // 8KB DMA buffer
    if (!buf) return;

    // SCSI READ(10) sector 0 (MBR or VBR).
    if (usb_msc_read(dev, 0, 0, buf, 1) < 0) {
        kprintf("[USB-MSC] self-test: READ(10) sector 0 FAILED\n");
        pmm_free_pages((uint64_t)buf, 2);
        return;
    }
    kprintf("[USB-MSC] self-test: READ(10) sector 0 OK, sig=%02x%02x\n",
            buf[510], buf[511]);

    // Locate the FAT volume: MBR partition table, else assume superfloppy @0.
    uint32_t part_start = 0;
    if (buf[510] == 0x55 && buf[511] == 0xAA && buf[450] != 0 &&
        rd16(buf + 11) == 0 /* not a BPB in sector 0 */) {
        part_start = rd32(buf + 454);
    }
    // Read the volume boot record (BPB).
    if (usb_msc_read(dev, 0, part_start, buf, 1) < 0) {
        kprintf("[USB-MSC] self-test: READ(10) VBR @%u FAILED\n", part_start);
        pmm_free_pages((uint64_t)buf, 2);
        return;
    }
    uint16_t bytes_per_sec = rd16(buf + 11);
    uint8_t  sec_per_clus  = buf[13];
    uint16_t rsvd          = rd16(buf + 14);
    uint8_t  num_fats      = buf[16];
    uint16_t root_ent      = rd16(buf + 17);
    uint16_t fatsz16       = rd16(buf + 22);
    uint32_t fatsz32       = rd32(buf + 36);
    if (bytes_per_sec == 0) bytes_per_sec = bs;

    kprintf("[USB-MSC] self-test: FAT BPB bps=%u spc=%u rsvd=%u fats=%u rootent=%u\n",
            bytes_per_sec, sec_per_clus, rsvd, num_fats, root_ent);

    uint32_t root_lba, root_sectors;
    if (root_ent != 0) {
        // FAT12/16: fixed root directory region.
        root_lba = part_start + rsvd + (uint32_t)num_fats * fatsz16;
        root_sectors = ((root_ent * 32) + bytes_per_sec - 1) / bytes_per_sec;
    } else {
        // FAT32: root is a cluster chain; read the first cluster.
        uint32_t root_clus = rd32(buf + 44);
        uint32_t first_data = rsvd + (uint32_t)num_fats * fatsz32;
        root_lba = part_start + first_data + (root_clus - 2) * sec_per_clus;
        root_sectors = sec_per_clus ? sec_per_clus : 1;
    }

    int listed = 0;
    uint32_t chunk = (root_sectors > 16) ? 16 : root_sectors;  // cap at 8KB buffer
    if (usb_msc_read(dev, 0, root_lba, buf, chunk) < 0) {
        kprintf("[USB-MSC] self-test: READ(10) root dir @%u FAILED\n", root_lba);
        pmm_free_pages((uint64_t)buf, 2);
        return;
    }
    kprintf("[USB-MSC] self-test: root directory @LBA %u:\n", root_lba);
    msc_list_dir_sector(buf, chunk * bytes_per_sec, &listed);
    kprintf("[USB-MSC] self-test: listed %d file(s) - USB Mass Storage READ path OK\n",
            listed);
    pmm_free_pages((uint64_t)buf, 2);
}

static void usb_msc_selftest_worker(void *arg) {
    (void)arg;
    proc_sleep(3500);   // let enumeration + desktop settle
    usb_msc_selftest();
}

void usb_msc_start_selftest(void) {
    proc_create("usb_msc_test", usb_msc_selftest_worker, NULL, 2 /*PRIO_NORMAL*/);
}
