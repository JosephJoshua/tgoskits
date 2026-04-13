/*
 * test_mmap_edge.c — Test mmap edge cases and validation.
 *
 * Known issues in StarryOS (kernel/src/syscall/mm/mmap.rs:90-132):
 *   1. fd check uses `fd <= 0` instead of `fd < 0` or `fd == -1`, so fd=0
 *      (stdin) without MAP_ANONYMOUS incorrectly fails.
 *   2. MAP_ANONYMOUS with fd > 0 is rejected. Linux ignores fd when
 *      MAP_ANONYMOUS is set.
 *   3. madvise and msync are complete no-ops.
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <sys/mman.h>

#define PAGE 4096

TEST_BEGIN("mmap_edge")

TEST("anon_private_basic") {
    /* Baseline: standard anonymous private mapping should work */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_basic;

    /* Should be zero-filled */
    unsigned char *bytes = (unsigned char *)p;
    int ok = 1;
    size_t i;
    for (i = 0; i < PAGE; i++) {
        if (bytes[i] != 0) { ok = 0; break; }
    }
    EXPECT_TRUE(ok);

    /* Should be writable */
    memset(p, 0xAB, PAGE);
    EXPECT_EQ(bytes[0], 0xAB);

    munmap(p, PAGE);
done_basic:;
} TEND

TEST("anon_with_fd_positive") {
    /* Linux: MAP_ANONYMOUS with fd > 0 should succeed (fd is ignored).
     * StarryOS BUG: check `ANONYMOUS != (fd <= 0)` rejects this because
     * ANONYMOUS=true but fd>0 means fd<=0 is false → true != false → EINVAL.
     *
     * Some programs pass a valid fd with MAP_ANONYMOUS (e.g., by accident
     * or for portability). Linux silently ignores the fd. */
    int fd = open("/tmp/mmap_test_file", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_anon_fd;

    write(fd, "testdata", 8);

    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap_edge::anon_with_fd_positive "
               "(MAP_ANONYMOUS with fd=%d rejected, errno=%d %s) at %s:%d\n",
               fd, errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        /* Mapping should be anonymous (zero-filled), not file-backed */
        unsigned char *bytes = (unsigned char *)p;
        EXPECT_EQ(bytes[0], 0);
        munmap(p, PAGE);
    }

    close(fd);
done_anon_fd:
    unlink("/tmp/mmap_test_file");
} TEND

TEST("file_backed_read") {
    /* Positive test: file-backed mmap for reading */
    int fd = open("/tmp/mmap_fb", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_fb;

    const char *data = "mmap_file_backed_test_data!!";
    write(fd, data, strlen(data));

    void *p = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap_edge::file_backed_read "
               "(file-backed mmap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        close(fd);
        goto done_fb;
    }

    EXPECT_TRUE(memcmp(p, data, strlen(data)) == 0);

    munmap(p, PAGE);
    close(fd);
done_fb:
    unlink("/tmp/mmap_fb");
} TEND

TEST("length_zero_fails") {
    /* mmap with length=0 should fail with EINVAL */
    void *p = mmap(NULL, 0, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_ERRNO((long)p, (long)MAP_FAILED, EINVAL);
} TEND

TEST("unaligned_offset_fails") {
    /* File-backed mmap with non-page-aligned offset should fail */
    int fd = open("/tmp/mmap_unalign", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_unalign;

    /* Write enough data */
    char buf[8192];
    memset(buf, 'X', sizeof(buf));
    write(fd, buf, sizeof(buf));

    void *p = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 100);
    EXPECT_ERRNO((long)p, (long)MAP_FAILED, EINVAL);

    close(fd);
done_unalign:
    unlink("/tmp/mmap_unalign");
} TEND

TEST("private_cow_isolation") {
    /* MAP_PRIVATE creates copy-on-write: writes should not affect the file */
    int fd = open("/tmp/mmap_cow", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_cow;

    write(fd, "ORIGINAL", 8);

    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: mmap_edge::private_cow_isolation "
               "(mmap failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        close(fd);
        goto done_cow;
    }

    /* Verify initial content */
    EXPECT_TRUE(memcmp(p, "ORIGINAL", 8) == 0);

    /* Write through the mapping */
    memcpy(p, "MODIFIED", 8);

    /* Sync and verify the file is NOT modified (private mapping) */
    munmap(p, PAGE);

    char buf[16] = {0};
    pread(fd, buf, 8, 0);
    if (memcmp(buf, "ORIGINAL", 8) != 0) {
        printf("FAIL: mmap_edge::private_cow_isolation "
               "(file content changed to '%.8s' — MAP_PRIVATE COW broken) at %s:%d\n",
               buf, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_cow:
    unlink("/tmp/mmap_cow");
} TEND

TEST("fixed_noreplace_on_existing") {
    /* MAP_FIXED_NOREPLACE should fail with EEXIST if address is already mapped */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_noreplace;

    /* Try to map at the same address with FIXED_NOREPLACE */
    void *p2 = mmap(p, PAGE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p2 != MAP_FAILED) {
        printf("FAIL: mmap_edge::fixed_noreplace_on_existing "
               "(MAP_FIXED_NOREPLACE succeeded on existing mapping, expected EEXIST) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
        if (p2 != p) munmap(p2, PAGE);
    } else {
        EXPECT_TRUE(errno == EEXIST);
    }

    munmap(p, PAGE);
done_noreplace:;
} TEND

TEST("shared_anon_mapping") {
    /* MAP_SHARED | MAP_ANONYMOUS should create a shared anonymous mapping */
    void *p = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    EXPECT_NE(p, MAP_FAILED);
    if (p == MAP_FAILED) goto done_shared;

    /* Should be zero-filled and writable */
    unsigned char *bytes = (unsigned char *)p;
    EXPECT_EQ(bytes[0], 0);
    bytes[0] = 42;
    EXPECT_EQ(bytes[0], 42);

    munmap(p, PAGE);
done_shared:;
} TEND

TEST_END
