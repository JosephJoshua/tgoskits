/*
 * test_memfd_create.c — Test memfd_create syscall correctness.
 *
 * Known issues in StarryOS (kernel/src/syscall/fs/memfd.rs:13-32):
 *   1. Creates real files at /tmp/memfd-XXXX instead of anonymous memory objects.
 *   2. No MFD_ALLOW_SEALING support.
 *   3. Files persist on disk after close (real memfd is anonymous, nlink=0).
 *   4. Sequential ID scan (0..0xFFFF) is racy under concurrent use.
 *   5. Unknown flags not rejected (should return EINVAL for unsupported flags).
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>

/* memfd_create may not be in musl, use syscall directly */
#ifndef SYS_memfd_create
#define SYS_memfd_create 279  /* riscv64 */
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING 0x0002U
#endif

static int my_memfd_create(const char *name, unsigned int flags) {
    return (int)syscall(SYS_memfd_create, name, flags);
}

TEST_BEGIN("memfd_create")

TEST("basic_create_and_readwrite") {
    /* Positive test: create memfd, write, seek, read back */
    int fd = my_memfd_create("basic_test", 0);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_basic;

    ssize_t n = write(fd, "hello memfd", 11);
    EXPECT_EQ(n, 11);

    lseek(fd, 0, SEEK_SET);

    char buf[32] = {0};
    ssize_t r = read(fd, buf, sizeof(buf));
    EXPECT_EQ(r, 11);
    EXPECT_TRUE(memcmp(buf, "hello memfd", 11) == 0);

    close(fd);
done_basic:;
} TEND

TEST("cloexec_flag") {
    /* MFD_CLOEXEC should set FD_CLOEXEC on the descriptor */
    int fd = my_memfd_create("cloexec_test", MFD_CLOEXEC);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_cloexec;

    int flags = fcntl(fd, F_GETFD);
    EXPECT_TRUE(flags >= 0);
    EXPECT_TRUE(flags & FD_CLOEXEC);

    close(fd);
done_cloexec:;
} TEND

TEST("no_cloexec_by_default") {
    /* Without MFD_CLOEXEC, FD_CLOEXEC should NOT be set */
    int fd = my_memfd_create("no_cloexec", 0);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_nocloexec;

    int flags = fcntl(fd, F_GETFD);
    EXPECT_TRUE(flags >= 0);
    if (flags & FD_CLOEXEC) {
        printf("FAIL: memfd_create::no_cloexec_by_default "
               "(FD_CLOEXEC set without MFD_CLOEXEC flag) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_nocloexec:;
} TEND

TEST("anonymous_nlink_zero") {
    /* A real memfd has nlink=0 (no directory entry).
     * StarryOS BUG: creates a real file at /tmp/memfd-XXXX, so nlink=1. */
    int fd = my_memfd_create("anon_test", 0);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_nlink;

    struct stat st;
    int ret = fstat(fd, &st);
    EXPECT_EQ(ret, 0);

    if (st.st_nlink != 0) {
        printf("FAIL: memfd_create::anonymous_nlink_zero "
               "(st_nlink = %lu, expected 0 — memfd is backed by a real file) at %s:%d\n",
               (unsigned long)st.st_nlink, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_nlink:;
} TEND

TEST("ftruncate_and_mmap") {
    /* memfd should support ftruncate to set size, and be mmappable */
    int fd = my_memfd_create("mmap_test", 0);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_mmap;

    int ret = ftruncate(fd, 4096);
    EXPECT_EQ(ret, 0);

    struct stat st;
    fstat(fd, &st);
    EXPECT_EQ(st.st_size, 4096);

    /* mmap the memfd */
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("FAIL: memfd_create::ftruncate_and_mmap "
               "(mmap on memfd failed, errno=%d %s) at %s:%d\n",
               errno, strerror(errno), __FILE__, __LINE__);
        _st_cur_ok = 0;
        close(fd);
        goto done_mmap;
    }

    /* Write through mmap */
    memset(p, 0xAB, 4096);

    /* Read back through fd */
    char buf[16];
    pread(fd, buf, 1, 0);
    EXPECT_EQ((unsigned char)buf[0], 0xAB);

    munmap(p, 4096);
    close(fd);
done_mmap:;
} TEND

TEST("data_not_persisted_after_close") {
    /* Real memfd: after closing all fds, the data is gone.
     * StarryOS BUG: the /tmp/memfd-XXXX file persists.
     * Test strategy: create memfd, write unique data, close, then
     * scan /tmp for files containing that data. */
    int fd = my_memfd_create("persist_test", 0);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_persist;

    /* Write unique marker */
    const char *marker = "UNIQUE_MEMFD_MARKER_12345";
    write(fd, marker, strlen(marker));
    close(fd);

    /* Now try to find this data by opening /tmp/memfd-* files */
    int found = 0;
    char path[64];
    int i;
    for (i = 0; i < 16 && !found; i++) {
        snprintf(path, sizeof(path), "/tmp/memfd-%04x", i);
        int check_fd = open(path, O_RDONLY);
        if (check_fd >= 0) {
            char buf[64] = {0};
            read(check_fd, buf, sizeof(buf));
            if (strstr(buf, "UNIQUE_MEMFD_MARKER") != NULL) {
                found = 1;
            }
            close(check_fd);
        }
    }

    if (found) {
        printf("FAIL: memfd_create::data_not_persisted_after_close "
               "(memfd data found in /tmp/memfd-* after close — not anonymous) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

done_persist:;
} TEND

TEST("independent_fds") {
    /* Two memfd_create calls should return independent file descriptors */
    int fd1 = my_memfd_create("test1", 0);
    int fd2 = my_memfd_create("test2", 0);
    EXPECT_TRUE(fd1 >= 0);
    EXPECT_TRUE(fd2 >= 0);
    if (fd1 < 0 || fd2 < 0) goto done_indep;

    EXPECT_TRUE(fd1 != fd2);

    write(fd1, "AAA", 3);
    write(fd2, "BBB", 3);

    char buf1[8] = {0};
    char buf2[8] = {0};
    pread(fd1, buf1, 3, 0);
    pread(fd2, buf2, 3, 0);

    EXPECT_TRUE(memcmp(buf1, "AAA", 3) == 0);
    EXPECT_TRUE(memcmp(buf2, "BBB", 3) == 0);

    close(fd1);
    close(fd2);
done_indep:;
} TEND

TEST_END
