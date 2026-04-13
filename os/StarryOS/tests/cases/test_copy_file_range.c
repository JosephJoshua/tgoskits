/*
 * test_copy_file_range.c — Test copy_file_range syscall correctness.
 *
 * Known issues in StarryOS (kernel/src/syscall/fs/io.rs:317-352):
 *   1. flags parameter ignored (Linux requires flags=0, any other → EINVAL)
 *   2. No check that both fds are regular files (pipes/sockets should → EINVAL)
 *   3. No same-file overlap detection — buffer-based copy (4096-byte chunks)
 *      causes data corruption for forward-overlapping same-file copies
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <sys/syscall.h>

/* musl may not provide copy_file_range wrapper */
static ssize_t my_copy_file_range(int fd_in, off_t *off_in,
                                   int fd_out, off_t *off_out,
                                   size_t len, unsigned int flags) {
    return syscall(SYS_copy_file_range, fd_in, off_in, fd_out, off_out, len, flags);
}

#define TEST_SRC "/tmp/cfr_src"
#define TEST_DST "/tmp/cfr_dst"
#define TEST_SAME "/tmp/cfr_same"

TEST_BEGIN("copy_file_range")

TEST("basic_copy_between_files") {
    /* Positive test: copy data from one file to another */
    int src = open(TEST_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(src >= 0);
    if (src < 0) goto done_basic;

    const char *data = "Hello, copy_file_range!";
    write(src, data, strlen(data));

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(src); goto done_basic; }

    off_t off_in = 0;
    off_t off_out = 0;
    ssize_t n = my_copy_file_range(src, &off_in, dst, &off_out, strlen(data), 0);
    EXPECT_EQ(n, (ssize_t)strlen(data));

    /* Verify offsets were updated */
    EXPECT_EQ(off_in, (off_t)strlen(data));
    EXPECT_EQ(off_out, (off_t)strlen(data));

    /* Read back and verify */
    char buf[64] = {0};
    pread(dst, buf, sizeof(buf), 0);
    EXPECT_TRUE(memcmp(buf, data, strlen(data)) == 0);

    close(src);
    close(dst);
done_basic:
    unlink(TEST_SRC);
    unlink(TEST_DST);
} TEND

TEST("copy_with_offsets") {
    /* Copy from middle of src to middle of dst */
    int src = open(TEST_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(src >= 0);
    if (src < 0) goto done_offsets;

    write(src, "AAAA_HELLO_BBBB", 15);

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(src); goto done_offsets; }

    write(dst, "XXXXXXXXXXXXXXX", 15);

    off_t off_in = 5;   /* start at "HELLO" */
    off_t off_out = 3;  /* write starting at dst[3] */
    ssize_t n = my_copy_file_range(src, &off_in, dst, &off_out, 5, 0);
    EXPECT_EQ(n, 5);

    /* dst should now be "XXXHELLOXXXXXXX" → wait, 5 bytes "HELLO" at offset 3 */
    char buf[32] = {0};
    pread(dst, buf, 15, 0);
    EXPECT_TRUE(memcmp(buf, "XXXHELLO_XXXXXX", 15) == 0);

    close(src);
    close(dst);
done_offsets:
    unlink(TEST_SRC);
    unlink(TEST_DST);
} TEND

TEST("flags_nonzero_should_fail") {
    /* Linux: flags must be 0. Any nonzero value → EINVAL.
     * StarryOS BUG: _flags parameter is ignored. */
    int src = open(TEST_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(src >= 0);
    if (src < 0) goto done_flags;

    write(src, "testdata", 8);

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(src); goto done_flags; }

    off_t off_in = 0;
    off_t off_out = 0;

    /* flags=1 should be rejected */
    ssize_t n = my_copy_file_range(src, &off_in, dst, &off_out, 8, 1);
    if (n >= 0) {
        printf("FAIL: copy_file_range::flags_nonzero_should_fail "
               "(accepted flags=1, returned %zd, expected EINVAL) at %s:%d\n",
               n, __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EINVAL);
    }

    close(src);
    close(dst);
done_flags:
    unlink(TEST_SRC);
    unlink(TEST_DST);
} TEND

TEST("same_file_forward_overlap_corruption") {
    /* Same file, forward overlap: copy bytes [0..5999] to [2000..7999].
     * The overlapping region [2000..5999] gets overwritten during the first
     * buffer write, then read back as corrupted data for the second read.
     *
     * StarryOS BUG: uses 4096-byte buffer in do_send, so:
     *   Step 1: read file[0..4095] into buffer
     *   Step 2: write buffer to file[2000..6095] → OVERWRITES file[4096..6095]
     *   Step 3: read file[4096..5999] → reads CORRUPTED data
     *   Step 4: write corrupted data to file[6096..7999]
     *
     * Result: file[6096..7999] contains wrong data.
     */
    int fd = open(TEST_SAME, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_overlap;

    /* Fill 8192 bytes with pattern: byte[i] = i & 0xFF */
    unsigned char pattern[8192];
    size_t i;
    for (i = 0; i < 8192; i++) {
        pattern[i] = (unsigned char)(i & 0xFF);
    }
    write(fd, pattern, 8192);

    /* Copy 6000 bytes from offset 0 to offset 2000 (same file, forward overlap) */
    off_t off_in = 0;
    off_t off_out = 2000;
    ssize_t n = my_copy_file_range(fd, &off_in, fd, &off_out, 6000, 0);

    if (n < 0) {
        /* If kernel rejects same-file overlap, that's actually correct Linux < 5.3 behavior */
        EXPECT_TRUE(errno == EINVAL);
        close(fd);
        goto done_overlap;
    }

    EXPECT_EQ(n, 6000);

    /* Read back the file */
    unsigned char result[8192];
    memset(result, 0, sizeof(result));
    pread(fd, result, 8192, 0);

    /* Build expected result: correct copy of original[0..5999] → file[2000..7999] */
    unsigned char expected[8192];
    memcpy(expected, pattern, 8192);
    memcpy(expected + 2000, pattern, 6000);

    /* Check the critical region: file[6096..7999] — this is where corruption happens.
     * The first 4096 bytes of the copy (file[2000..6095]) should be correct because
     * they come from the first buffer read before any overwrites.
     * But file[6096..7999] comes from the second read, after file[4096..6095] was
     * already overwritten. */
    int corruption_found = 0;
    for (i = 6096; i < 7999 && i < 8192; i++) {
        if (result[i] != expected[i]) {
            if (!corruption_found) {
                printf("FAIL: copy_file_range::same_file_forward_overlap_corruption "
                       "(byte %zu: got 0x%02x, expected 0x%02x — data corrupted) at %s:%d\n",
                       i, result[i], expected[i], __FILE__, __LINE__);
            }
            corruption_found = 1;
        }
    }
    if (corruption_found) {
        _st_cur_ok = 0;
    }

    close(fd);
done_overlap:
    unlink(TEST_SAME);
} TEND

TEST("copy_zero_length") {
    /* Copying 0 bytes should succeed and return 0 */
    int src = open(TEST_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(src >= 0);
    if (src < 0) goto done_zero;

    write(src, "data", 4);

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(src); goto done_zero; }

    off_t off_in = 0;
    off_t off_out = 0;
    ssize_t n = my_copy_file_range(src, &off_in, dst, &off_out, 0, 0);
    EXPECT_EQ(n, 0);

    close(src);
    close(dst);
done_zero:
    unlink(TEST_SRC);
    unlink(TEST_DST);
} TEND

TEST("copy_past_eof") {
    /* Copy starting past EOF should return 0 (no data to copy) */
    int src = open(TEST_SRC, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(src >= 0);
    if (src < 0) goto done_eof;

    write(src, "short", 5);

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(src); goto done_eof; }

    off_t off_in = 100;  /* past EOF */
    off_t off_out = 0;
    ssize_t n = my_copy_file_range(src, &off_in, dst, &off_out, 1024, 0);
    EXPECT_EQ(n, 0);

    close(src);
    close(dst);
done_eof:
    unlink(TEST_SRC);
    unlink(TEST_DST);
} TEND

TEST("pipe_should_fail") {
    /* Linux: copy_file_range requires regular files. Pipes → EINVAL.
     * StarryOS BUG: delegates to do_send which accepts any FileLike. */
    int pipefd[2];
    int ret = pipe(pipefd);
    EXPECT_EQ(ret, 0);
    if (ret != 0) goto done_pipe;

    int dst = open(TEST_DST, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(dst >= 0);
    if (dst < 0) { close(pipefd[0]); close(pipefd[1]); goto done_pipe; }

    /* Write some data to pipe */
    write(pipefd[1], "pipedata", 8);

    off_t off_out = 0;
    ssize_t n = my_copy_file_range(pipefd[0], NULL, dst, &off_out, 8, 0);
    if (n >= 0) {
        printf("FAIL: copy_file_range::pipe_should_fail "
               "(accepted pipe as input, returned %zd, expected EINVAL) at %s:%d\n",
               n, __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EINVAL);
    }

    close(pipefd[0]);
    close(pipefd[1]);
    close(dst);
done_pipe:
    unlink(TEST_DST);
} TEND

TEST_END
