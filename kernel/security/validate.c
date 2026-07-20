// validate.c - Pointer and Memory Validation implementation for MayteraOS
#include "../mm/vmm.h"  // Include vmm.h first to get its definitions
#include "validate.h"
#include "../serial.h"
#include "../string.h"

// ============================================================================
// Module State
// ============================================================================

static bool g_validate_initialized = false;
static uint64_t g_validation_failures = 0;

// ============================================================================
// Initialization
// ============================================================================

void validate_init(void) {
    g_validate_initialized = true;
    g_validation_failures = 0;
    kprintf("[VALIDATE] Pointer validation subsystem initialized\n");
}

// ============================================================================
// Address Space Checks
// ============================================================================

bool is_canonical_address(uint64_t addr) {
    // In x86-64, addresses must be canonical:
    // Bits 48-63 must all be copies of bit 47
    // This means either all 0s (user space) or all 1s (kernel space)
    uint64_t high_bits = addr >> 47;
    return (high_bits == 0) || (high_bits == 0x1FFFF);
}

bool is_user_address(uint64_t addr) {
    // User addresses: 0x0000000000000000 - 0x00007FFFFFFFFFFF
    // But exclude null page for safety
    return (addr >= NULL_PAGE_END) && (addr <= USER_SPACE_END);
}

bool is_kernel_address(uint64_t addr) {
    // Kernel addresses: 0xFFFF800000000000 - 0xFFFFFFFFFFFFFFFF
    return (addr >= KERNEL_SPACE_START);
}

// ============================================================================
// Error Strings
// ============================================================================

const char *validate_error_string(validate_error_t error) {
    switch (error) {
        case VALIDATE_OK:                   return "OK";
        case VALIDATE_NULL:                 return "Null pointer";
        case VALIDATE_UNALIGNED:            return "Unaligned pointer";
        case VALIDATE_NON_CANONICAL:        return "Non-canonical address";
        case VALIDATE_KERNEL_SPACE:         return "Kernel space access from user";
        case VALIDATE_UNMAPPED:             return "Address not mapped";
        case VALIDATE_NO_READ:              return "Page not readable";
        case VALIDATE_NO_WRITE:             return "Page not writable";
        case VALIDATE_NO_EXEC:              return "Page not executable";
        case VALIDATE_NO_USER:              return "Page not user-accessible";
        case VALIDATE_OVERFLOW:             return "Address range overflow";
        case VALIDATE_MMIO:                 return "MMIO region access";
        case VALIDATE_STRING_UNTERMINATED:  return "String not null-terminated";
        case VALIDATE_ARRAY_TOO_LARGE:      return "Array too large";
        default:                            return "Unknown error";
    }
}

// ============================================================================
// Core Validation Functions
// ============================================================================

validate_error_t validate_alignment(const void *ptr, size_t type_size) {
    if (type_size == 0 || type_size == 1) {
        return VALIDATE_OK;  // No alignment required
    }

    // Type size should be power of 2 for natural alignment
    size_t alignment = type_size;
    if (alignment > 16) alignment = 16;  // Cap at 16-byte alignment

    if (!is_aligned(ptr, alignment)) {
        return VALIDATE_UNALIGNED;
    }

    return VALIDATE_OK;
}

validate_error_t validate_user_ptr(const void *ptr, size_t size, uint32_t access) {
    uint64_t addr = (uint64_t)ptr;
    uint64_t end_addr;

    // Check for null pointer
    if (ptr == NULL) {
        g_validation_failures++;
        return VALIDATE_NULL;
    }

    // Check for size = 0 (valid but meaningless)
    if (size == 0) {
        return VALIDATE_OK;
    }

    // Check for overflow
    if (__builtin_add_overflow(addr, size - 1, &end_addr)) {
        g_validation_failures++;
        return VALIDATE_OVERFLOW;
    }

    // Check canonical form
    if (!is_canonical_address(addr) || !is_canonical_address(end_addr)) {
        g_validation_failures++;
        return VALIDATE_NON_CANONICAL;
    }

    // Reject the upper (kernel) half unconditionally.
    //
    // #500. This check used to be gated on (access & ACCESS_USER), so a caller
    // that passed ACCESS_READ without ACCESS_USER skipped it entirely. The
    // function is named validate_USER_ptr; user-ness is not optional. The gate
    // is gone.
    //
    // NOTE this range test is necessary but NOWHERE NEAR sufficient on this OS:
    // MayteraOS identity-maps the kernel into the LOWER half at 0x400000, so
    // kernel text/heap have LOW addresses that sail straight through any
    // "is it below the kernel half?" test. The U/S bit check below is what
    // actually separates user memory from kernel memory here.
    if (!is_user_address(addr) || !is_user_address(end_addr)) {
        g_validation_failures++;
        return VALIDATE_KERNEL_SPACE;
    }

    // Check for null page access
    if (addr < NULL_PAGE_END) {
        g_validation_failures++;
        return VALIDATE_NULL;
    }

    // Check for MMIO region (should not be accessed by user)
    if (addr >= MMIO_REGION_START && addr < MMIO_REGION_END) {
        g_validation_failures++;
        return VALIDATE_MMIO;
    }
    if (end_addr >= MMIO_REGION_START && end_addr < MMIO_REGION_END) {
        g_validation_failures++;
        return VALIDATE_MMIO;
    }

    // Check that memory is actually mapped, IN THE CALLER'S ADDRESS SPACE.
    //
    // #487 BUG FIX. This loop used to call vmm_is_mapped()/vmm_get_physical(),
    // which walk `kernel_pml4` - the KERNEL's page-table root. A user pointer
    // (apps link at 0x80000000) lives in the calling process's OWN PML4 lower
    // half, which kernel_pml4 does not contain, so this walk reported EVERY
    // valid user pointer as VALIDATE_UNMAPPED. In other words this function
    // rejected exactly the pointers it exists to accept.
    //
    // That is almost certainly WHY it had zero callers anywhere in the tree
    // (it is unusable as written), which in turn is why the whole syscall
    // surface validates nothing. Fixed by walking the CURRENT address space via
    // vmm_get_physical_in(cr3, ...). Blast radius is nil: this function had no
    // callers to break. The first callers are the #487 introspection syscalls
    // (proc/procinfo.c), which is how the bug was found: every one of them
    // returned -1 and the Task Manager's Services/Details tabs rendered empty.
    //
    // CR3 is read directly rather than trusting vmm_get_pml4()'s cached
    // current_pml4_phys: a context switch can write CR3 without going through
    // vmm_switch_pml4(), so the cache can be stale, whereas CR3 is by
    // definition the address space we are executing in.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= VMM_ADDR_MASK;   // strip PCID / flag bits

    // #500 / MAYTERA-SEC-2026-0016. THE load-bearing check.
    //
    // The #487 fix made this walk the CALLER's address space instead of the
    // (empty) kernel_pml4, which stopped it rejecting every valid pointer. But
    // it still asked only "is this page PRESENT?" - and on this OS that question
    // does not distinguish user memory from kernel memory at all:
    //
    //   - the kernel is identity-mapped (phys == virt) at KERNEL_PHYS_BASE
    //     = 0x400000, i.e. a LOW address, in the LOWER half;
    //   - vmm_create_user_space() copies PML4[0] into EVERY user address space
    //     (it must: that is where the kernel and the user's own 2GB code live);
    //   - so kernel text at 0x400000 is PRESENT in the calling process's CR3,
    //     and 0x400000 passes is_user_address() because it is < 2^47.
    //
    // A presence-only check therefore returned VALIDATE_OK for a Ring-3 pointer
    // naming kernel text, kernel heap, or another process's page tables. It
    // accepted precisely the pointers it exists to reject. Rolling that version
    // out across the syscall surface would have bought no security whatsoever
    // while looking like it had.
    //
    // What separates user from kernel here is the U/S bit, and only the U/S bit.
    // vmm_get_effective_flags_in() ANDs it across all four levels exactly as the
    // hardware does, so we accept a page if and only if the CPU would have let
    // Ring 3 touch it directly. For a write we likewise demand the effective R/W
    // bit, so a read-only user page (e.g. a COW page, or .rodata) can no longer
    // be used as a syscall output buffer.
    bool need_write = (access & ACCESS_WRITE) != 0;

    uint64_t page_addr = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t page_end = ALIGN_UP(end_addr + 1, PAGE_SIZE);

    // Every page in the WHOLE range, not just the base. A valid base with an
    // attacker-chosen length is the classic bypass; the loop is the point.
    while (page_addr < page_end) {
        uint64_t eff = vmm_get_effective_flags_in(cr3, page_addr);
        if (!(eff & VMM_FLAG_PRESENT)) {
            g_validation_failures++;
            return VALIDATE_UNMAPPED;
        }
        if (!(eff & VMM_FLAG_USER)) {
            // Present, but Ring 3 could not reach it with a plain mov. That
            // makes it kernel memory, whatever its numeric address looks like.
            g_validation_failures++;
            return VALIDATE_NO_USER;
        }
        if (need_write && !(eff & VMM_FLAG_WRITABLE)) {
            g_validation_failures++;
            return VALIDATE_NO_WRITE;
        }
        page_addr += PAGE_SIZE;
    }

    return VALIDATE_OK;
}

validate_error_t validate_user_string(const char *str, size_t max_len) {
    uint64_t addr = (uint64_t)str;

    // Check for null pointer
    if (str == NULL) {
        g_validation_failures++;
        return VALIDATE_NULL;
    }

    // Check maximum length is reasonable
    if (max_len == 0) {
        return VALIDATE_OK;  // Empty string is valid
    }

    if (max_len > 1024 * 1024) {  // 1MB max string length
        g_validation_failures++;
        return VALIDATE_ARRAY_TOO_LARGE;
    }

    // Check if start address is in user space
    if (!is_user_address(addr)) {
        g_validation_failures++;
        return VALIDATE_KERNEL_SPACE;
    }

    // #500 / MAYTERA-SEC-2026-0016. This function had the ORIGINAL #487 bug and
    // the #487 fix did NOT reach it: it still called vmm_is_mapped(), which
    // walks the static kernel_pml4 array. vmm_init() memsets kernel_pml4 to zero
    // ("for future use") and NEVER loads it into CR3 - the kernel runs on UEFI's
    // page tables throughout. So vmm_is_mapped() answers "no" for essentially
    // every address in existence, kernel ones included, and this function
    // returned VALIDATE_UNMAPPED for every string ever passed to it. That is why
    // strncpy_from_user()/strnlen_user() were unusable, and it is why
    // sys_svc_control() (procinfo.c) rejects every name it is given today.
    //
    // Walk the CALLER's address space, and demand the U/S bit, for the same
    // reason as validate_user_ptr(): a low address is not evidence of user
    // memory on an OS that identity-maps its kernel at 0x400000.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= VMM_ADDR_MASK;

    uint64_t current_page = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t eff = vmm_get_effective_flags_in(cr3, current_page);
    bool page_valid = (eff & VMM_FLAG_PRESENT) && (eff & VMM_FLAG_USER);

    for (size_t i = 0; i < max_len; i++) {
        uint64_t char_addr = addr + i;

        // Check if we crossed to a new page. The re-check on EVERY page
        // crossing is what stops a string that starts in a legitimate user page
        // and runs on into kernel memory.
        uint64_t char_page = ALIGN_DOWN(char_addr, PAGE_SIZE);
        if (char_page != current_page) {
            current_page = char_page;
            eff = vmm_get_effective_flags_in(cr3, current_page);
            page_valid = (eff & VMM_FLAG_PRESENT) && (eff & VMM_FLAG_USER);
        }

        // Page must be present AND user-accessible before we dereference it.
        if (!page_valid) {
            g_validation_failures++;
            return (eff & VMM_FLAG_PRESENT) ? VALIDATE_NO_USER : VALIDATE_UNMAPPED;
        }

        // Check if in user space
        if (!is_user_address(char_addr)) {
            g_validation_failures++;
            return VALIDATE_KERNEL_SPACE;
        }

        // Check for null terminator
        if (str[i] == '\0') {
            return VALIDATE_OK;
        }
    }

    // Reached max_len without finding null terminator
    g_validation_failures++;
    return VALIDATE_STRING_UNTERMINATED;
}

validate_error_t validate_user_array(const void *arr, size_t count, size_t elem_size, uint32_t access) {
    // Check for null pointer
    if (arr == NULL && count > 0) {
        g_validation_failures++;
        return VALIDATE_NULL;
    }

    // Check for empty array (valid)
    if (count == 0 || elem_size == 0) {
        return VALIDATE_OK;
    }

    // Check for integer overflow in total size
    size_t total_size;
    if (__builtin_mul_overflow(count, elem_size, &total_size)) {
        g_validation_failures++;
        return VALIDATE_OVERFLOW;
    }

    // Reasonable limit check (1GB max array)
    if (total_size > 1024 * 1024 * 1024) {
        g_validation_failures++;
        return VALIDATE_ARRAY_TOO_LARGE;
    }

    // Validate as pointer with calculated size
    return validate_user_ptr(arr, total_size, access);
}

validate_error_t validate_kernel_ptr(const void *ptr, size_t size) {
    uint64_t addr = (uint64_t)ptr;
    uint64_t end_addr;

    // Check for null pointer
    if (ptr == NULL) {
        return VALIDATE_NULL;
    }

    // Check for size = 0
    if (size == 0) {
        return VALIDATE_OK;
    }

    // Check for overflow
    if (__builtin_add_overflow(addr, size - 1, &end_addr)) {
        return VALIDATE_OVERFLOW;
    }

    // Check canonical form
    if (!is_canonical_address(addr) || !is_canonical_address(end_addr)) {
        return VALIDATE_NON_CANONICAL;
    }

    // For kernel pointers, we allow both kernel space and user space
    // (kernel can access everything), so no U/S requirement here.

    // #500. Was vmm_is_mapped(), which walks the always-empty kernel_pml4 and so
    // reported EVERY address as unmapped - including ordinary kernel heap
    // pointers. That single call is why copy_from_user(), copy_to_user(),
    // strncpy_from_user() and clear_user() returned -EFAULT unconditionally for
    // every argument they were ever given, which is why the entire safe-copy API
    // has zero callers. Walk the live address space (CR3) instead.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    cr3 &= VMM_ADDR_MASK;

    uint64_t page_addr = ALIGN_DOWN(addr, PAGE_SIZE);
    uint64_t page_end = ALIGN_UP(end_addr + 1, PAGE_SIZE);

    while (page_addr < page_end) {
        if (!(vmm_get_effective_flags_in(cr3, page_addr) & VMM_FLAG_PRESENT)) {
            return VALIDATE_UNMAPPED;
        }
        page_addr += PAGE_SIZE;
    }

    return VALIDATE_OK;
}

// ============================================================================
// Safe Copy Functions
// ============================================================================

int copy_from_user(void *dest, const void *src, size_t size) {
    // Validate destination (kernel pointer)
    validate_error_t err = validate_kernel_ptr(dest, size);
    if (err != VALIDATE_OK) {
        kprintf("[VALIDATE] copy_from_user: invalid dest - %s\n", validate_error_string(err));
        return -14;  // EFAULT
    }

    // Validate source (user pointer, read access)
    err = validate_user_ptr(src, size, ACCESS_READ_USER);
    if (err != VALIDATE_OK) {
        kprintf("[VALIDATE] copy_from_user: invalid src - %s\n", validate_error_string(err));
        return -14;  // EFAULT
    }

    // Perform the copy
    memcpy(dest, src, size);
    return 0;
}

int copy_to_user(void *dest, const void *src, size_t size) {
    // Validate destination (user pointer, write access)
    validate_error_t err = validate_user_ptr(dest, size, ACCESS_WRITE_USER);
    if (err != VALIDATE_OK) {
        kprintf("[VALIDATE] copy_to_user: invalid dest - %s\n", validate_error_string(err));
        return -14;  // EFAULT
    }

    // Validate source (kernel pointer)
    err = validate_kernel_ptr(src, size);
    if (err != VALIDATE_OK) {
        kprintf("[VALIDATE] copy_to_user: invalid src - %s\n", validate_error_string(err));
        return -14;  // EFAULT
    }

    // Perform the copy
    memcpy(dest, src, size);
    return 0;
}

ssize_t strncpy_from_user(char *dest, const char *src, size_t max_len) {
    // Validate destination (kernel pointer)
    validate_error_t err = validate_kernel_ptr(dest, max_len);
    if (err != VALIDATE_OK) {
        return -14;  // EFAULT
    }

    // Validate source string
    err = validate_user_string(src, max_len);
    if (err != VALIDATE_OK) {
        return -14;  // EFAULT
    }

    // Copy the string
    size_t i;
    for (i = 0; i < max_len - 1 && src[i] != '\0'; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';

    return (ssize_t)i;
}

ssize_t strnlen_user(const char *str, size_t max_len) {
    // Validate the string
    validate_error_t err = validate_user_string(str, max_len);
    if (err != VALIDATE_OK) {
        return -14;  // EFAULT
    }

    // Count the length
    size_t len = 0;
    while (len < max_len && str[len] != '\0') {
        len++;
    }

    return (ssize_t)len;
}

int clear_user(void *dest, size_t size) {
    // Validate destination (user pointer, write access)
    validate_error_t err = validate_user_ptr(dest, size, ACCESS_WRITE_USER);
    if (err != VALIDATE_OK) {
        return -14;  // EFAULT
    }

    // Clear the memory
    memset(dest, 0, size);
    return 0;
}
