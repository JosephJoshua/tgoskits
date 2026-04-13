/*
 * test_flock.c — Test flock (file locking) syscall correctness.
 *
 * Known issue in StarryOS (kernel/src/syscall/fs/fd_ops.rs:308-312):
 *   flock is a complete stub: always returns Ok(0) regardless of arguments.
 *   No actual locking is performed, so:
 *   - Exclusive locks don't exclude other lockers
 *   - fcntl F_SETLK/F_SETLKW are also stubs
 *   - F_GETLK always reports F_UNLCK (no locks)
 */
#define _GNU_SOURCE
#include "starry_test.h"
#include <sys/file.h>

#define TEST_FILE "/tmp/test_flock"

TEST_BEGIN("flock")

TEST("basic_exclusive_lock_unlock") {
    /* Positive test: lock and unlock should both succeed */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_basic;

    int ret = flock(fd, LOCK_EX);
    EXPECT_EQ(ret, 0);

    ret = flock(fd, LOCK_UN);
    EXPECT_EQ(ret, 0);

    close(fd);
done_basic:
    unlink(TEST_FILE);
} TEND

TEST("basic_shared_lock_unlock") {
    /* Shared lock and unlock */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_shared;

    int ret = flock(fd, LOCK_SH);
    EXPECT_EQ(ret, 0);

    ret = flock(fd, LOCK_UN);
    EXPECT_EQ(ret, 0);

    close(fd);
done_shared:
    unlink(TEST_FILE);
} TEND

TEST("exclusive_blocks_exclusive") {
    /* Two separate open()s on the same file: first locks LOCK_EX,
     * second tries LOCK_EX|LOCK_NB — should fail with EWOULDBLOCK.
     * StarryOS BUG: flock stub always returns 0, so second lock "succeeds". */
    int fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd1 >= 0);
    if (fd1 < 0) goto done_ex_ex;

    int fd2 = open(TEST_FILE, O_RDWR);
    EXPECT_TRUE(fd2 >= 0);
    if (fd2 < 0) { close(fd1); goto done_ex_ex; }

    /* First fd: acquire exclusive lock */
    int ret = flock(fd1, LOCK_EX);
    EXPECT_EQ(ret, 0);

    /* Second fd: try non-blocking exclusive lock — should fail */
    ret = flock(fd2, LOCK_EX | LOCK_NB);
    if (ret == 0) {
        printf("FAIL: flock::exclusive_blocks_exclusive "
               "(second LOCK_EX|LOCK_NB succeeded — no actual locking) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN);
    }

    flock(fd1, LOCK_UN);
    close(fd1);
    close(fd2);
done_ex_ex:
    unlink(TEST_FILE);
} TEND

TEST("exclusive_blocks_shared") {
    /* Exclusive lock should block a shared lock attempt.
     * StarryOS BUG: stub always returns 0. */
    int fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd1 >= 0);
    if (fd1 < 0) goto done_ex_sh;

    int fd2 = open(TEST_FILE, O_RDWR);
    EXPECT_TRUE(fd2 >= 0);
    if (fd2 < 0) { close(fd1); goto done_ex_sh; }

    flock(fd1, LOCK_EX);

    /* Try shared lock — should fail because fd1 holds exclusive */
    int ret = flock(fd2, LOCK_SH | LOCK_NB);
    if (ret == 0) {
        printf("FAIL: flock::exclusive_blocks_shared "
               "(LOCK_SH succeeded while LOCK_EX held — no actual locking) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN);
    }

    flock(fd1, LOCK_UN);
    close(fd1);
    close(fd2);
done_ex_sh:
    unlink(TEST_FILE);
} TEND

TEST("shared_allows_shared") {
    /* Multiple shared locks should coexist without conflict */
    int fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd1 >= 0);
    if (fd1 < 0) goto done_sh_sh;

    int fd2 = open(TEST_FILE, O_RDWR);
    EXPECT_TRUE(fd2 >= 0);
    if (fd2 < 0) { close(fd1); goto done_sh_sh; }

    int ret1 = flock(fd1, LOCK_SH);
    EXPECT_EQ(ret1, 0);

    /* Second shared lock should also succeed */
    int ret2 = flock(fd2, LOCK_SH | LOCK_NB);
    EXPECT_EQ(ret2, 0);

    flock(fd1, LOCK_UN);
    flock(fd2, LOCK_UN);
    close(fd1);
    close(fd2);
done_sh_sh:
    unlink(TEST_FILE);
} TEND

TEST("shared_blocks_exclusive") {
    /* Active shared lock should block exclusive lock.
     * StarryOS BUG: no actual locking. */
    int fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd1 >= 0);
    if (fd1 < 0) goto done_sh_ex;

    int fd2 = open(TEST_FILE, O_RDWR);
    EXPECT_TRUE(fd2 >= 0);
    if (fd2 < 0) { close(fd1); goto done_sh_ex; }

    flock(fd1, LOCK_SH);

    /* Exclusive lock should fail while shared lock is held */
    int ret = flock(fd2, LOCK_EX | LOCK_NB);
    if (ret == 0) {
        printf("FAIL: flock::shared_blocks_exclusive "
               "(LOCK_EX succeeded while LOCK_SH held — no actual locking) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EWOULDBLOCK || errno == EAGAIN);
    }

    flock(fd1, LOCK_UN);
    close(fd1);
    close(fd2);
done_sh_ex:
    unlink(TEST_FILE);
} TEND

TEST("unlock_allows_relock") {
    /* After unlock, another locker should be able to acquire */
    int fd1 = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd1 >= 0);
    if (fd1 < 0) goto done_relock;

    int fd2 = open(TEST_FILE, O_RDWR);
    EXPECT_TRUE(fd2 >= 0);
    if (fd2 < 0) { close(fd1); goto done_relock; }

    /* Lock and unlock fd1 */
    flock(fd1, LOCK_EX);
    flock(fd1, LOCK_UN);

    /* fd2 should now be able to lock */
    int ret = flock(fd2, LOCK_EX | LOCK_NB);
    EXPECT_EQ(ret, 0);

    flock(fd2, LOCK_UN);
    close(fd1);
    close(fd2);
done_relock:
    unlink(TEST_FILE);
} TEND

TEST("invalid_operation") {
    /* Invalid operation value should fail with EINVAL.
     * StarryOS BUG: stub doesn't validate operation argument. */
    int fd = open(TEST_FILE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    EXPECT_TRUE(fd >= 0);
    if (fd < 0) goto done_invalid;

    int ret = flock(fd, 0);  /* 0 is not a valid operation */
    if (ret == 0) {
        printf("FAIL: flock::invalid_operation "
               "(flock(fd, 0) succeeded — should fail with EINVAL) at %s:%d\n",
               __FILE__, __LINE__);
        _st_cur_ok = 0;
    } else {
        EXPECT_TRUE(errno == EINVAL);
    }

    close(fd);
done_invalid:
    unlink(TEST_FILE);
} TEND

TEST_END
