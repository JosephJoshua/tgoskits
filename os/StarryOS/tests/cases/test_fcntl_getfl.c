/*
 * test_fcntl_getfl.c — Test fcntl F_GETFL / F_SETFL correctness.
 *
 * Known bug in StarryOS (kernel/src/syscall/fs/fd_ops.rs:256-273):
 *   F_GETFL derives the access mode (O_RDONLY/O_WRONLY/O_RDWR) from the
 *   file's permission bits (stat.mode) instead of the flags passed to open().
 *   This means a 0644 file always reports O_RDWR regardless of how it was
 *   opened. Also, O_APPEND is completely lost.
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <fcntl.h>

#define TEST_FILE "/tmp/test_fcntl"

/* O_ACCMODE is the mask for access mode bits (O_RDONLY|O_WRONLY|O_RDWR) */
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif

TEST_BEGIN("fcntl_getfl")

TEST("rdonly_on_rw_file") {
    /* Open a 0644 (rw-r--r--) file as O_RDONLY.
     * F_GETFL should report O_RDONLY (0).
     * StarryOS BUG: reports O_RDWR because file has owner rw permission. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_rdonly;
    close(fd);

    fd = open(TEST_FILE, O_RDONLY);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_rdonly;

    int flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);

    int accmode = flags & O_ACCMODE;
    if (accmode != O_RDONLY) {
        printf("FAIL: fcntl_getfl::rdonly_on_rw_file "
               "(F_GETFL accmode = %d, expected O_RDONLY = %d) at %s:%d\n",
               accmode, O_RDONLY, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_rdonly:
    unlink(TEST_FILE);
} TEND

TEST("wronly_on_rw_file") {
    /* Open a 0644 file as O_WRONLY.
     * F_GETFL should report O_WRONLY (1).
     * StarryOS BUG: reports O_RDWR because file has owner rw permission. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_wronly;
    close(fd);

    fd = open(TEST_FILE, O_WRONLY);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_wronly;

    int flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);

    int accmode = flags & O_ACCMODE;
    if (accmode != O_WRONLY) {
        printf("FAIL: fcntl_getfl::wronly_on_rw_file "
               "(F_GETFL accmode = %d, expected O_WRONLY = %d) at %s:%d\n",
               accmode, O_WRONLY, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_wronly:
    unlink(TEST_FILE);
} TEND

TEST("rdwr_on_rw_file") {
    /* Open a 0644 file as O_RDWR.
     * F_GETFL should report O_RDWR (2).
     * This may PASS by coincidence — file has rw permissions → returns O_RDWR. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_rdwr;

    int flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);

    int accmode = flags & O_ACCMODE;
    EXPECT_EQ(accmode, O_RDWR);

    close(fd);
done_rdwr:
    unlink(TEST_FILE);
} TEND

TEST("append_flag_preserved") {
    /* Open with O_WRONLY | O_APPEND.
     * F_GETFL should include O_APPEND.
     * StarryOS BUG: O_APPEND is never tracked, so it's lost. */
    int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_append;

    int flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);

    if (!(flags & O_APPEND)) {
        printf("FAIL: fcntl_getfl::append_flag_preserved "
               "(F_GETFL = 0x%x, missing O_APPEND = 0x%x) at %s:%d\n",
               flags, O_APPEND, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_append:
    unlink(TEST_FILE);
} TEND

TEST("setfl_nonblock_roundtrip") {
    /* Positive test: set O_NONBLOCK via F_SETFL, verify via F_GETFL.
     * This should PASS since StarryOS tracks nonblocking state. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_nb;

    /* Verify initially not nonblock */
    int flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);
    EXPECT_TRUE(!(flags & O_NONBLOCK));

    /* Set nonblock */
    int ret = fcntl(fd, F_SETFL, O_NONBLOCK);
    EXPECT_EQ(ret, 0);

    /* Verify it's set */
    flags = fcntl(fd, F_GETFL);
    EXPECT_TRUE(flags >= 0);
    if (!(flags & O_NONBLOCK)) {
        printf("FAIL: fcntl_getfl::setfl_nonblock_roundtrip "
               "(F_GETFL = 0x%x after F_SETFL O_NONBLOCK) at %s:%d\n",
               flags, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_nb:
    unlink(TEST_FILE);
} TEND

TEST("setfl_append_via_fcntl") {
    /* F_SETFL should be able to toggle O_APPEND.
     * StarryOS BUG: F_SETFL only handles O_NONBLOCK, ignores O_APPEND. */
    int fd = open(TEST_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_setappend;

    /* Set O_APPEND via F_SETFL */
    int ret = fcntl(fd, F_SETFL, O_APPEND);
    EXPECT_EQ(ret, 0);

    /* Write some data — should go to end due to O_APPEND */
    const char *data1 = "AAAA";
    write(fd, data1, 4);

    /* Seek to beginning and write again — with O_APPEND, data should
     * still be appended, not overwrite beginning */
    lseek(fd, 0, SEEK_SET);
    const char *data2 = "BBBB";
    write(fd, data2, 4);

    /* Read back and verify: if O_APPEND worked, file should be "AAAABBBB".
     * If O_APPEND was ignored, file should be "BBBB" (overwritten). */
    close(fd);
    fd = open(TEST_FILE, O_RDONLY);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_setappend;

    char buf[16] = {0};
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n == 8 && memcmp(buf, "AAAABBBB", 8) == 0) {
        /* O_APPEND works correctly */
    } else if (n == 4 && memcmp(buf, "BBBB", 4) == 0) {
        printf("FAIL: fcntl_getfl::setfl_append_via_fcntl "
               "(F_SETFL O_APPEND had no effect — data overwritten) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        printf("FAIL: fcntl_getfl::setfl_append_via_fcntl "
               "(unexpected file content, len=%zd) at %s:%d\n",
               n, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_setappend:
    unlink(TEST_FILE);
} TEND

TEST("getfl_pipe_read_end") {
    /* Pipe read end should have O_RDONLY access mode.
     * Tests F_GETFL on non-regular-file descriptors. */
    int pipefd[2];
    int ret = pipe(pipefd);
    EXPECT_EQ(ret, 0);
    if (ret != 0) goto done_pipe;

    int flags = fcntl(pipefd[0], F_GETFL);
    EXPECT_TRUE(flags >= 0);

    int accmode = flags & O_ACCMODE;
    if (accmode != O_RDONLY) {
        printf("FAIL: fcntl_getfl::getfl_pipe_read_end "
               "(pipe read end accmode = %d, expected O_RDONLY = %d) at %s:%d\n",
               accmode, O_RDONLY, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(pipefd[0]);
    close(pipefd[1]);
done_pipe:;
} TEND

TEST("getfl_pipe_write_end") {
    /* Pipe write end should have O_WRONLY access mode. */
    int pipefd[2];
    int ret = pipe(pipefd);
    EXPECT_EQ(ret, 0);
    if (ret != 0) goto done_pipew;

    int flags = fcntl(pipefd[1], F_GETFL);
    EXPECT_TRUE(flags >= 0);

    int accmode = flags & O_ACCMODE;
    if (accmode != O_WRONLY) {
        printf("FAIL: fcntl_getfl::getfl_pipe_write_end "
               "(pipe write end accmode = %d, expected O_WRONLY = %d) at %s:%d\n",
               accmode, O_WRONLY, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(pipefd[0]);
    close(pipefd[1]);
done_pipew:;
} TEND

TEST("accmode_not_changed_by_setfl") {
    /* F_SETFL cannot change the access mode bits (O_ACCMODE).
     * Attempting to set O_RDWR on a O_RDONLY fd should be ignored. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_nochange;
    close(fd);

    fd = open(TEST_FILE, O_RDONLY);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_nochange;

    /* Try to "upgrade" to O_RDWR via F_SETFL — should be ignored */
    fcntl(fd, F_SETFL, O_RDWR);

    int flags = fcntl(fd, F_GETFL);
    int accmode = flags & O_ACCMODE;

    /* Access mode should still be O_RDONLY */
    if (accmode != O_RDONLY) {
        printf("FAIL: fcntl_getfl::accmode_not_changed_by_setfl "
               "(accmode = %d after F_SETFL O_RDWR, expected O_RDONLY = %d) at %s:%d\n",
               accmode, O_RDONLY, __FILE__, __LINE__);
        _st_cur_ok = 0;
    }

    close(fd);
done_nochange:
    unlink(TEST_FILE);
} TEND

TEST_END
