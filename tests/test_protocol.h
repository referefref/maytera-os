/**
 * MayteraOS Test Protocol Header
 *
 * This header defines the protocol for communication between
 * the test runner and in-OS test applications.
 *
 * Test Protocol Format:
 * - Test commands are sent via serial: "TEST:<test_name>"
 * - Results are returned via serial:
 *   - "TEST_PASS:<test_name>" for success
 *   - "TEST_FAIL:<test_name>:<error_msg>" for failure
 *   - "TEST_SKIP:<test_name>:<reason>" for skipped tests
 *   - "TEST_COMPLETE" when test execution finishes
 *
 * Assertion Macros:
 * - ASSERT_TRUE(cond) - Assert condition is true
 * - ASSERT_FALSE(cond) - Assert condition is false
 * - ASSERT_EQ(a, b) - Assert a equals b
 * - ASSERT_NE(a, b) - Assert a not equals b
 * - ASSERT_GT(a, b) - Assert a greater than b
 * - ASSERT_LT(a, b) - Assert a less than b
 * - ASSERT_NOT_NULL(ptr) - Assert pointer is not NULL
 */

#ifndef _TEST_PROTOCOL_H
#define _TEST_PROTOCOL_H

#include <stdint.h>

// Test result codes
typedef enum {
    TEST_RESULT_PASS = 0,
    TEST_RESULT_FAIL = 1,
    TEST_RESULT_SKIP = 2,
    TEST_RESULT_ERROR = 3,
    TEST_RESULT_TIMEOUT = 4
} test_result_t;

// Test case structure
typedef struct {
    const char *name;
    const char *description;
    test_result_t (*test_func)(void);
    int timeout_ms;
} test_case_t;

// Test suite structure
typedef struct {
    const char *name;
    test_case_t *tests;
    int test_count;
} test_suite_t;

// Current test name (for error reporting)
extern const char *_current_test_name;
extern int _test_assertions_passed;
extern int _test_assertions_failed;

// Test output functions (implemented by the OS)
extern void kprintf(const char *fmt, ...);

// ===========================================================================
// Assertion Macros
// ===========================================================================

#define TEST_START(name) \
    _current_test_name = name; \
    _test_assertions_passed = 0; \
    _test_assertions_failed = 0; \
    kprintf("[TEST] Starting: %s\n", name);

#define TEST_END() \
    if (_test_assertions_failed == 0) { \
        kprintf("TEST_PASS:%s\n", _current_test_name); \
        return TEST_RESULT_PASS; \
    } else { \
        kprintf("TEST_FAIL:%s:assertions failed\n", _current_test_name); \
        return TEST_RESULT_FAIL; \
    }

#define ASSERT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            kprintf("TEST_FAIL:%s:ASSERT_TRUE failed at %s:%d\n", \
                    _current_test_name, __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_FALSE(cond) \
    do { \
        if (cond) { \
            kprintf("TEST_FAIL:%s:ASSERT_FALSE failed at %s:%d\n", \
                    _current_test_name, __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_EQ(a, b) \
    do { \
        if ((a) != (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_EQ failed (%ld != %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_NE(a, b) \
    do { \
        if ((a) == (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_NE failed (%ld == %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_GT(a, b) \
    do { \
        if ((a) <= (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_GT failed (%ld <= %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_LT(a, b) \
    do { \
        if ((a) >= (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_LT failed (%ld >= %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_GE(a, b) \
    do { \
        if ((a) < (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_GE failed (%ld < %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_LE(a, b) \
    do { \
        if ((a) > (b)) { \
            kprintf("TEST_FAIL:%s:ASSERT_LE failed (%ld > %ld) at %s:%d\n", \
                    _current_test_name, (long)(a), (long)(b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_NOT_NULL(ptr) \
    do { \
        if ((ptr) == NULL) { \
            kprintf("TEST_FAIL:%s:ASSERT_NOT_NULL failed at %s:%d\n", \
                    _current_test_name, __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_NULL(ptr) \
    do { \
        if ((ptr) != NULL) { \
            kprintf("TEST_FAIL:%s:ASSERT_NULL failed at %s:%d\n", \
                    _current_test_name, __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_STR_EQ(a, b) \
    do { \
        if (strcmp((a), (b)) != 0) { \
            kprintf("TEST_FAIL:%s:ASSERT_STR_EQ failed ('%s' != '%s') at %s:%d\n", \
                    _current_test_name, (a), (b), __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define ASSERT_MEM_EQ(a, b, size) \
    do { \
        if (memcmp((a), (b), (size)) != 0) { \
            kprintf("TEST_FAIL:%s:ASSERT_MEM_EQ failed at %s:%d\n", \
                    _current_test_name, __FILE__, __LINE__); \
            _test_assertions_failed++; \
            return TEST_RESULT_FAIL; \
        } else { \
            _test_assertions_passed++; \
        } \
    } while(0)

#define TEST_SKIP(reason) \
    do { \
        kprintf("TEST_SKIP:%s:%s\n", _current_test_name, reason); \
        return TEST_RESULT_SKIP; \
    } while(0)

// ===========================================================================
// Test Runner Functions
// ===========================================================================

/**
 * Run a single test case
 */
static inline test_result_t run_test(test_case_t *test) {
    if (!test || !test->test_func) {
        return TEST_RESULT_ERROR;
    }
    return test->test_func();
}

/**
 * Run all tests in a suite
 */
static inline int run_test_suite(test_suite_t *suite) {
    if (!suite || !suite->tests) {
        return -1;
    }

    int passed = 0;
    int failed = 0;
    int skipped = 0;

    kprintf("\n[TEST SUITE] %s (%d tests)\n", suite->name, suite->test_count);
    kprintf("============================================\n");

    for (int i = 0; i < suite->test_count; i++) {
        test_case_t *test = &suite->tests[i];
        test_result_t result = run_test(test);

        switch (result) {
            case TEST_RESULT_PASS:
                passed++;
                break;
            case TEST_RESULT_FAIL:
            case TEST_RESULT_ERROR:
            case TEST_RESULT_TIMEOUT:
                failed++;
                break;
            case TEST_RESULT_SKIP:
                skipped++;
                break;
        }
    }

    kprintf("============================================\n");
    kprintf("[SUITE RESULT] %s: %d passed, %d failed, %d skipped\n",
            suite->name, passed, failed, skipped);
    kprintf("TEST_COMPLETE\n");

    return failed;
}

#endif /* _TEST_PROTOCOL_H */
