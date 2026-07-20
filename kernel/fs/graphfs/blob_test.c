// blob_test.c - Test functions for GraphFS Blob Store
// Part of MayteraOS - The First LLM-Native Operating System

#include "blob.h"
#include "../../string.h"
#include "../../serial.h"
#include "../../mm/heap.h"

// Test data
static const char test_data_1[] = "Hello, GraphFS! This is test data for the blob store.";
static const char test_data_2[] = "Another piece of content to test deduplication.";
static const char test_data_3[] = "Hello, GraphFS! This is test data for the blob store."; // Same as test_data_1

// ============================================================================
// Test Functions
// ============================================================================

// Test basic store and load operations
static int test_store_load(blob_store_t *store) {
    kprintf("[BLOB TEST] Testing store and load...\n");

    blob_hash_t hash;
    int ret = blob_store(store, test_data_1, sizeof(test_data_1) - 1, &hash);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: blob_store returned %d\n", ret);
        return -1;
    }

    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(&hash, hex);
    kprintf("[BLOB TEST] Stored blob: %s\n", hex);

    // Load the blob back
    char buffer[256];
    size_t actual_size;
    ret = blob_load(store, &hash, buffer, sizeof(buffer), &actual_size);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: blob_load returned %d\n", ret);
        return -1;
    }

    if (actual_size != sizeof(test_data_1) - 1) {
        kprintf("[BLOB TEST] FAIL: size mismatch: %zu vs %zu\n",
                actual_size, sizeof(test_data_1) - 1);
        return -1;
    }

    if (memcmp(buffer, test_data_1, actual_size) != 0) {
        kprintf("[BLOB TEST] FAIL: data mismatch\n");
        return -1;
    }

    kprintf("[BLOB TEST] PASS: Store and load\n");
    return 0;
}

// Test deduplication
static int test_deduplication(blob_store_t *store) {
    kprintf("[BLOB TEST] Testing deduplication...\n");

    uint64_t savings_before = 0;
    blob_store_stats(store, NULL, NULL, &savings_before);

    blob_hash_t hash1, hash3;

    // Store test_data_1
    int ret = blob_store(store, test_data_1, sizeof(test_data_1) - 1, &hash1);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: first store returned %d\n", ret);
        return -1;
    }

    // Store test_data_3 (identical to test_data_1)
    ret = blob_store(store, test_data_3, sizeof(test_data_3) - 1, &hash3);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: second store returned %d\n", ret);
        return -1;
    }

    // Hashes should be identical
    if (blob_hash_compare(&hash1, &hash3) != 0) {
        kprintf("[BLOB TEST] FAIL: hashes differ for identical content\n");
        return -1;
    }

    uint64_t savings_after = 0;
    blob_store_stats(store, NULL, NULL, &savings_after);

    if (savings_after <= savings_before) {
        kprintf("[BLOB TEST] FAIL: dedup savings not recorded\n");
        return -1;
    }

    kprintf("[BLOB TEST] PASS: Deduplication (saved %llu bytes)\n",
            savings_after - savings_before);
    return 0;
}

// Test reference counting
static int test_refcount(blob_store_t *store) {
    kprintf("[BLOB TEST] Testing reference counting...\n");

    blob_hash_t hash;
    int ret = blob_store(store, test_data_2, sizeof(test_data_2) - 1, &hash);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: store returned %d\n", ret);
        return -1;
    }

    // Initial refcount should be 1
    int refcount = blob_get_refcount(store, &hash);
    if (refcount != 1) {
        kprintf("[BLOB TEST] FAIL: initial refcount is %d, expected 1\n", refcount);
        return -1;
    }

    // Increment
    refcount = blob_ref(store, &hash);
    if (refcount != 2) {
        kprintf("[BLOB TEST] FAIL: refcount after ref is %d, expected 2\n", refcount);
        return -1;
    }

    // Decrement
    refcount = blob_unref(store, &hash);
    if (refcount != 1) {
        kprintf("[BLOB TEST] FAIL: refcount after unref is %d, expected 1\n", refcount);
        return -1;
    }

    kprintf("[BLOB TEST] PASS: Reference counting\n");
    return 0;
}

// Test existence check
static int test_exists(blob_store_t *store) {
    kprintf("[BLOB TEST] Testing existence check...\n");

    blob_hash_t hash;
    int ret = blob_store(store, test_data_1, sizeof(test_data_1) - 1, &hash);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: store returned %d\n", ret);
        return -1;
    }

    // Blob should exist
    ret = blob_exists(store, &hash);
    if (ret != 1) {
        kprintf("[BLOB TEST] FAIL: exists returned %d for existing blob\n", ret);
        return -1;
    }

    // Non-existent blob
    blob_hash_t fake_hash;
    blob_hash_clear(&fake_hash);
    fake_hash.bytes[0] = 0xFF;  // Make it non-zero

    ret = blob_exists(store, &fake_hash);
    if (ret != 0) {
        kprintf("[BLOB TEST] FAIL: exists returned %d for non-existent blob\n", ret);
        return -1;
    }

    kprintf("[BLOB TEST] PASS: Existence check\n");
    return 0;
}

// Test hash utilities
static int test_hash_utils(void) {
    kprintf("[BLOB TEST] Testing hash utilities...\n");

    // Test hash computation
    blob_hash_t hash;
    blob_hash_compute("test", 4, &hash);

    // Convert to hex
    char hex[BLOB_HASH_HEX_SIZE];
    blob_hash_to_hex(&hash, hex);

    // Expected SHA-256 of "test"
    // 9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08
    const char *expected = "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08";

    if (strcmp(hex, expected) != 0) {
        kprintf("[BLOB TEST] FAIL: hash mismatch\n");
        kprintf("[BLOB TEST]   Got:      %s\n", hex);
        kprintf("[BLOB TEST]   Expected: %s\n", expected);
        return -1;
    }

    // Test round-trip
    blob_hash_t parsed;
    int ret = blob_hash_from_hex(hex, &parsed);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] FAIL: parse returned %d\n", ret);
        return -1;
    }

    if (blob_hash_compare(&hash, &parsed) != 0) {
        kprintf("[BLOB TEST] FAIL: round-trip failed\n");
        return -1;
    }

    // Test zero hash
    blob_hash_t zero;
    blob_hash_clear(&zero);
    if (!blob_hash_is_zero(&zero)) {
        kprintf("[BLOB TEST] FAIL: zero hash not detected\n");
        return -1;
    }

    if (blob_hash_is_zero(&hash)) {
        kprintf("[BLOB TEST] FAIL: non-zero hash detected as zero\n");
        return -1;
    }

    kprintf("[BLOB TEST] PASS: Hash utilities\n");
    return 0;
}

// ============================================================================
// Main Test Runner
// ============================================================================

int blob_store_run_tests(blob_store_t *store) {
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  GraphFS Blob Store Test Suite\n");
    kprintf("========================================\n");
    kprintf("\n");

    int total = 0;
    int passed = 0;
    int failed = 0;

    // Test hash utilities (doesn't need store)
    total++;
    if (test_hash_utils() == 0) passed++; else failed++;

    if (!store || !store->initialized) {
        kprintf("[BLOB TEST] Store not initialized, skipping store tests\n");
        goto summary;
    }

    // Test store/load
    total++;
    if (test_store_load(store) == 0) passed++; else failed++;

    // Test deduplication
    total++;
    if (test_deduplication(store) == 0) passed++; else failed++;

    // Test reference counting
    total++;
    if (test_refcount(store) == 0) passed++; else failed++;

    // Test existence check
    total++;
    if (test_exists(store) == 0) passed++; else failed++;

summary:
    kprintf("\n");
    kprintf("========================================\n");
    kprintf("  Results: %d/%d passed, %d failed\n", passed, total, failed);
    kprintf("========================================\n");
    kprintf("\n");

    // Print store stats
    if (store && store->initialized) {
        blob_store_print_info(store);
    }

    return failed;
}

// Initialize and test (convenience function)
int blob_store_self_test(fat_fs_t *fs, const char *base_path) {
    blob_store_t store;

    kprintf("[BLOB TEST] Initializing blob store for self-test...\n");

    int ret = blob_store_init(&store, fs, base_path);
    if (ret != BLOB_OK) {
        kprintf("[BLOB TEST] Failed to initialize store: %d\n", ret);
        return -1;
    }

    int failures = blob_store_run_tests(&store);

    blob_store_shutdown(&store);

    return failures;
}
