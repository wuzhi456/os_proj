#include "fs_score_common.h"

#define FD_PATH "/score_fd"

static char buf[64];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(FD_PATH);

    int fd = open(FD_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", FD_PATH, fd);
    memmove(buf, "0123456789abcdef", 16);
    write_full(fd, buf, 16);
    close_ok(fd);

    int fd1 = open(FD_PATH, O_RDWR);
    int fd2 = open(FD_PATH, O_RDWR);
    CHECK(fd1 >= 0 && fd2 >= 0, "open two fds fd1=%d fd2=%d", fd1, fd2);

    memset(buf, 0, sizeof(buf));
    read_full(fd1, buf, 4);
    CHECK(memcmp(buf, "0123", 4) == 0, "fd1 first read mismatch");

    memset(buf, 0, sizeof(buf));
    read_full(fd2, buf, 4);
    CHECK(memcmp(buf, "0123", 4) == 0, "fd2 offset is not independent");

    memmove(buf, "AA", 2);
    write_full(fd1, buf, 2);
    CHECK(lseek(fd1, 0, SEEK_CUR) == 6, "fd1 SEEK_CUR mismatch after write");

    CHECK(lseek(fd2, 0, SEEK_SET) == 0, "fd2 seek reset failed");
    memset(buf, 0, sizeof(buf));
    read_full(fd2, buf, 8);
    CHECK(memcmp(buf, "0123AA67", 8) == 0, "fd2 did not see fd1 write");

    CHECK(lseek(fd1, 0, SEEK_END) == 16, "SEEK_END size mismatch");
    CHECK(lseek(fd2, 14, SEEK_SET) == 14, "seek near eof failed");
    memset(buf, 0, sizeof(buf));
    read_full(fd2, buf, 2);
    CHECK(memcmp(buf, "ef", 2) == 0, "read near eof mismatch");
    CHECK(read(fd2, buf, 1) == 0, "read past eof should return 0");

    close_ok(fd1);
    CHECK(read(fd1, buf, 1) < 0, "read on closed fd unexpectedly succeeded");
    close_ok(fd2);

    CHECK(close(999) < 0, "close invalid fd unexpectedly succeeded");
    CHECK(read(999, buf, 1) < 0, "read invalid fd unexpectedly succeeded");
    CHECK(write(999, buf, 1) < 0, "write invalid fd unexpectedly succeeded");
    CHECK(lseek(999, 0, SEEK_SET) < 0, "lseek invalid fd unexpectedly succeeded");

    CHECK(unlink(FD_PATH) == 0, "unlink %s failed", FD_PATH);
    pass("fs_score_fd");
    return 0;
}
