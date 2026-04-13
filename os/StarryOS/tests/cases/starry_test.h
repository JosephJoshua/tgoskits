/*
 * starry_test.h — Minimal test harness for StarryOS syscall testing.
 *
 * Usage:
 *   TEST_BEGIN("suite_name")
 *   TEST("test_name", { ... EXPECT_*(...); ... })
 *   TEST_END
 *
 * Output format (parsed by pipeline):
 *   PASS: suite_name::test_name
 *   FAIL: suite_name::test_name (expected ..., got ...)
 */
#ifndef STARRY_TEST_H
#define STARRY_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

static int _st_pass = 0;
static int _st_fail = 0;
static const char *_st_suite = "";
static int _st_cur_ok = 1;

#define TEST_BEGIN(name) \
    int main(void) { \
        _st_suite = name; \
        printf("=== StarryTest: %s ===\n", _st_suite);

#define TEST_END \
        printf("=== %s: %d passed, %d failed ===\n", \
               _st_suite, _st_pass, _st_fail); \
        return _st_fail > 0 ? 1 : 0; \
    }

/*
 * TEST(name) { body } TEND
 * Split into TEST/TEND to avoid C preprocessor comma-in-braces issues.
 */
#define TEST(name) \
    do { \
        const char *_st_cur_name_hack = name; \
        _st_cur_ok = 1; \
        errno = 0;

#define TEND \
        if (_st_cur_ok) { \
            printf("PASS: %s::%s\n", _st_suite, _st_cur_name_hack); \
            _st_pass++; \
        } else { \
            _st_fail++; \
        } \
    } while (0);

/* Expect val != bad */
#define EXPECT_NE(val, bad) \
    do { \
        if ((long)(val) == (long)(bad)) { \
            printf("FAIL: %s::%s (got %ld, did not expect %ld, errno=%d %s) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, \
                   (long)(val), (long)(bad), errno, strerror(errno), \
                   __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } \
    } while (0)

/* Expect val == expected */
#define EXPECT_EQ(val, expected) \
    do { \
        long _v = (long)(val); \
        long _e = (long)(expected); \
        if (_v != _e) { \
            printf("FAIL: %s::%s (got %ld, expected %ld, errno=%d %s) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, \
                   _v, _e, errno, strerror(errno), \
                   __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } \
    } while (0)

/* Expect operation to fail with specific errno */
#define EXPECT_ERRNO(val, fail_val, expected_errno) \
    do { \
        long _v = (long)(val); \
        int _err = errno; \
        if (_v != (long)(fail_val)) { \
            printf("FAIL: %s::%s (expected failure, got %ld) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, _v, __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } else if (_err != (expected_errno)) { \
            printf("FAIL: %s::%s (expected errno %d=%s, got %d=%s) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, \
                   (expected_errno), strerror(expected_errno), \
                   _err, strerror(_err), __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } \
    } while (0)

/* Expect val >= 0 (success) */
#define EXPECT_OK(val) \
    do { \
        long _v = (long)(val); \
        if (_v < 0) { \
            printf("FAIL: %s::%s (got %ld, expected >= 0, errno=%d %s) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, \
                   _v, errno, strerror(errno), __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } \
    } while (0)

/* Expect boolean condition to be true */
#define EXPECT_TRUE(cond) \
    do { \
        if (!(cond)) { \
            printf("FAIL: %s::%s (%s was false) at %s:%d\n", \
                   _st_suite, _st_cur_name_hack, \
                   #cond, __FILE__, __LINE__); \
            _st_cur_ok = 0; \
        } \
    } while (0)

#endif /* STARRY_TEST_H */
