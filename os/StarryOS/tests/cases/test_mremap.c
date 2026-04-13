/*
 * test_mremap.c — Test mremap syscall correctness.
 *
 * Known bugs in StarryOS mremap (kernel/src/syscall/mm/mmap.rs:282-318):
 *   1. CRITICAL: Internal sys_mmap call uses MAP_PRIVATE without MAP_ANONYMOUS
 *      but passes fd=-1, failing the (ANONYMOUS != fd<=0) check → EINVAL always.
 *   2. Variable shadowing: user's MREMAP_* flags overwritten by MappingFlags.
 *   3. Always moves: creates new mapping + copies, even without MREMAP_MAYMOVE.
 *   4. Always PRIVATE ANONYMOUS: ignores original mapping type (shared/file).
 *   5. Shrink always moves too: Linux should return same address for shrinks.
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <sys/mman.h>

/* musl may not define these */
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif

#define PAGE 4096

TEST_BEGIN("mremap")

TEST("mremap_not_einval") {
    /* Most basic test: can mremap succeed at all?
     * StarryOS BUG: sys_mremap calls sys_mmap with MAP_PRIVATE but
     * no MAP_ANONYMOUS, and fd=-1. sys_mmap rejects this with EINVAL.
     * So ALL mremap calls fail unconditionally. */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_basic;

    memset(p, 0xAB, PAGE);

    void *p2 = mremap(p, PAGE, 2 * PAGE, MREMAP_MAYMOVE);
    EXPECT_NE(p2, MAP_FAILED);

    if (p2 != MAP_FAILED) {
        munmap(p2, 2 * PAGE);
    } else {
        munmap(p, PAGE);
    }
done_basic:;
} TEND

TEST("basic_grow_data_preserved") {
    /* Grow from 1 page to 2 pages, verify original data is intact */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_grow;

    memset(p, 0xAB, PAGE);

    void *p2 = mremap(p, PAGE, 2 * PAGE, MREMAP_MAYMOVE);
    if (p2 == MAP_FAILED) {
        printf("FAIL: mremap::basic_grow_data_preserved "
               "(mremap returned MAP_FAILED, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        munmap(p, PAGE);
        goto done_grow;
    }

    /* Original data must be preserved */
    unsigned char *bytes = (unsigned char *)p2;
    int ok = 1;
    size_t i;
    for (i = 0; i < PAGE; i++) {
        if (bytes[i] != 0xAB) { ok = 0; break; }
    }
    EXPECT_TRUE(ok);

    munmap(p2, 2 * PAGE);
done_grow:;
} TEND

TEST("grow_new_pages_zeroed") {
    /* After growing, new pages beyond old_size should be zero-filled */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_zero;

    memset(p, 0xFF, PAGE);

    void *p2 = mremap(p, PAGE, 3 * PAGE, MREMAP_MAYMOVE);
    if (p2 == MAP_FAILED) {
        printf("FAIL: mremap::grow_new_pages_zeroed "
               "(mremap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        munmap(p, PAGE);
        goto done_zero;
    }

    /* New pages (page 1 and 2) should be zero */
    unsigned char *bytes = (unsigned char *)p2;
    int ok = 1;
    size_t i;
    for (i = PAGE; i < 3 * PAGE; i++) {
        if (bytes[i] != 0) {
            printf("FAIL: mremap::grow_new_pages_zeroed "
                   "(byte %zu: got 0x%02x, expected 0x00) at %s:%d\n",
                   i, bytes[i], __FILE__, __LINE__);
            ok = 0;
            break;
        }
    }
    EXPECT_TRUE(ok);

    munmap(p2, 3 * PAGE);
done_zero:;
} TEND

TEST("basic_shrink") {
    /* Shrink from 4 pages to 1 page */
    void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_shrink;

    memset(p, 0xCD, 4 * PAGE);

    void *p2 = mremap(p, 4 * PAGE, PAGE, 0);
    if (p2 == MAP_FAILED) {
        printf("FAIL: mremap::basic_shrink "
               "(mremap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        munmap(p, 4 * PAGE);
        goto done_shrink;
    }

    /* Surviving data should be intact */
    unsigned char *bytes = (unsigned char *)p2;
    int ok = 1;
    size_t i;
    for (i = 0; i < PAGE; i++) {
        if (bytes[i] != 0xCD) { ok = 0; break; }
    }
    EXPECT_TRUE(ok);

    munmap(p2, PAGE);
done_shrink:;
} TEND

TEST("shrink_returns_same_addr") {
    /* Linux: shrinking must return the same address. StarryOS BUG:
     * always creates a new mapping and may return a different address. */
    void *p = mmap(NULL, 4 * PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_shrink_addr;

    void *p2 = mremap(p, 4 * PAGE, PAGE, 0);
    if (p2 == MAP_FAILED) {
        printf("FAIL: mremap::shrink_returns_same_addr "
               "(mremap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        munmap(p, 4 * PAGE);
        goto done_shrink_addr;
    }

    if (p2 != p) {
        printf("FAIL: mremap::shrink_returns_same_addr "
               "(shrink moved %p -> %p, should be same address) at %s:%d\n",
               p, p2, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    munmap(p2, PAGE);
done_shrink_addr:;
} TEND

TEST("no_maymove_must_not_move") {
    /* Without MREMAP_MAYMOVE, grow should either succeed in-place
     * (same address) or fail with ENOMEM. Moving is not allowed.
     * StarryOS BUG: ignores MREMAP_* flags entirely. */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_nomove;

    memset(p, 0x42, PAGE);

    /* Try to grow 4x WITHOUT MREMAP_MAYMOVE */
    void *p2 = mremap(p, PAGE, 4 * PAGE, 0);

    if (p2 != MAP_FAILED) {
        /* If succeeded, must be same address (in-place only) */
        if (p2 != p) {
            printf("FAIL: mremap::no_maymove_must_not_move "
                   "(moved from %p to %p without MREMAP_MAYMOVE) at %s:%d\n",
                   p, p2, __FILE__, __LINE__);
            _st_cur_ok = 0;
            munmap(p2, 4 * PAGE);
        } else {
            /* In-place grow succeeded — verify data */
            unsigned char *bytes = (unsigned char *)p2;
            int ok = 1;
            size_t i;
            for (i = 0; i < PAGE; i++) {
                if (bytes[i] != 0x42) { ok = 0; break; }
            }
            EXPECT_TRUE(ok);
            munmap(p2, 4 * PAGE);
        }
    } else {
        /* ENOMEM is acceptable — can't grow in-place */
        EXPECT_TRUE(errno == ENOMEM);
        munmap(p, PAGE);
    }
done_nomove:;
} TEND

TEST("data_pattern_integrity") {
    /* Write a sequential byte pattern, grow, verify every byte exactly */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_pattern;

    unsigned char *bytes = (unsigned char *)p;
    size_t i;
    for (i = 0; i < PAGE; i++) {
        bytes[i] = (unsigned char)(i & 0xFF);
    }

    void *p2 = mremap(p, PAGE, 3 * PAGE, MREMAP_MAYMOVE);
    if (p2 == MAP_FAILED) {
        printf("FAIL: mremap::data_pattern_integrity "
               "(mremap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        munmap(p, PAGE);
        goto done_pattern;
    }

    bytes = (unsigned char *)p2;
    int ok = 1;
    for (i = 0; i < PAGE; i++) {
        if (bytes[i] != (unsigned char)(i & 0xFF)) {
            printf("FAIL: mremap::data_pattern_integrity "
                   "(byte %zu: got 0x%02x, expected 0x%02x) at %s:%d\n",
                   i, bytes[i], (unsigned char)(i & 0xFF),
                   __FILE__, __LINE__);
            ok = 0;
            break;
        }
    }
    EXPECT_TRUE(ok);

    munmap(p2, 3 * PAGE);
done_pattern:;
} TEND

TEST("invalid_unaligned_addr") {
    /* Non-page-aligned address should fail with EINVAL */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_unaligned;

    void *bad = mremap((char *)p + 1, PAGE, PAGE, MREMAP_MAYMOVE);
    EXPECT_ERRNO((long)bad, (long)MAP_FAILED, EINVAL);

    munmap(p, PAGE);
done_unaligned:;
} TEND

TEST("invalid_zero_new_size") {
    /* new_size = 0 should fail with EINVAL */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_zerosize;

    void *bad = mremap(p, PAGE, 0, MREMAP_MAYMOVE);
    EXPECT_ERRNO((long)bad, (long)MAP_FAILED, EINVAL);

    munmap(p, PAGE);
done_zerosize:;
} TEND

TEST_END
