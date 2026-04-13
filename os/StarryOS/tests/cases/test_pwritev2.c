/*
 * test_pwritev2.c — Verify pwritev/pwritev2 actually writes data.
 *
 * Known bug: sys_pwritev2 (kernel/src/syscall/fs/io.rs:220) calls read_at
 * instead of write_at, so data is silently NOT written.
 */
#include "starry_test.h"
#include <sys/uio.h>
#include <sys/syscall.h>

/* musl doesn't provide pwritev2 wrapper, call it directly */
static ssize_t my_pwritev2(int fd, const struct iovec *iov, int iovcnt,
                           off_t offset, int flags) {
    return syscall(SYS_pwritev2, fd, iov, iovcnt, offset, flags);
}

TEST_BEGIN("pwritev2")

TEST("pwrite_basic") {
    /* Baseline: pwrite (not pwritev) should work */
    int fd = open("/tmp/test_pwrite", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);

    const char *data = "hello";
    ssize_t n = pwrite(fd, data, 5, 0);
    EXPECT_EQ(n, 5);

    char buf[8] = {0};
    ssize_t r = pread(fd, buf, 5, 0);
    EXPECT_EQ(r, 5);
    EXPECT_TRUE(memcmp(buf, "hello", 5) == 0);

    close(fd);
    unlink("/tmp/test_pwrite");
} TEND

TEST("pwritev_writes_data") {
    /* This tests pwritev which delegates to pwritev2 — should FAIL due to bug */
    int fd = open("/tmp/test_pwritev", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);

    /* First, write some data with pwrite so the file has content */
    const char *zeros = "00000000";
    pwrite(fd, zeros, 8, 0);

    /* Now overwrite via pwritev */
    char data1[] = "AAAA";
    char data2[] = "BBBB";
    struct iovec iov[2];
    iov[0].iov_base = data1;
    iov[0].iov_len = 4;
    iov[1].iov_base = data2;
    iov[1].iov_len = 4;

    ssize_t n = pwritev(fd, iov, 2, 0);
    EXPECT_EQ(n, 8);

    /* Read back and check if data was actually written */
    char buf[16] = {0};
    ssize_t r = pread(fd, buf, 8, 0);
    EXPECT_EQ(r, 8);
    EXPECT_TRUE(memcmp(buf, "AAAABBBB", 8) == 0);

    close(fd);
    unlink("/tmp/test_pwritev");
} TEND

TEST("pwritev2_writes_data") {
    /* Direct pwritev2 test — should also FAIL due to bug */
    int fd = open("/tmp/test_pwritev2", O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);

    /* Pre-fill with known pattern */
    const char *zeros = "00000000";
    pwrite(fd, zeros, 8, 0);

    /* Overwrite via pwritev2 with flags=0 */
    char data[] = "TESTDATA";
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = 8;

    ssize_t n = my_pwritev2(fd, &iov, 1, 0, 0);
    EXPECT_EQ(n, 8);

    /* Read back */
    char buf[16] = {0};
    ssize_t r = pread(fd, buf, 8, 0);
    EXPECT_EQ(r, 8);
    EXPECT_TRUE(memcmp(buf, "TESTDATA", 8) == 0);

    close(fd);
    unlink("/tmp/test_pwritev2");
} TEND

TEST_END
