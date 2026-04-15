#include "fs_score_common.h"

#define RW_PATH "/score_rw"
#define RW_SIZE (SCORE_BLOCK_SIZE * 2)

static char data[RW_SIZE];
static char got[RW_SIZE];
static char patch[64];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(RW_PATH);

    int fd = open(RW_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", RW_PATH, fd);

    fill_pattern(data, sizeof(data), 31);
    write_full(fd, data, sizeof(data));

    fill_pattern(patch, 32, 77);
    write_at_full(fd, SCORE_BLOCK_SIZE - 6, patch, 32);
    memmove(data + SCORE_BLOCK_SIZE - 6, patch, 32);

    memset(got, 0, sizeof(got));
    read_at_full(fd, SCORE_BLOCK_SIZE - 16, got, 64);
    CHECK(memcmp(got, data + SCORE_BLOCK_SIZE - 16, 64) == 0, "cross-block overwrite mismatch");

    CHECK(lseek(fd, RW_SIZE - 10, SEEK_SET) == RW_SIZE - 10, "seek partial eof failed");
    memset(got, 0, sizeof(got));
    int n = read(fd, got, 32);
    CHECK(n == 10, "partial eof read n=%d expected=10", n);
    CHECK(memcmp(got, data + RW_SIZE - 10, 10) == 0, "partial eof data mismatch");
    CHECK(read(fd, got, 1) == 0, "eof read should return 0");

    CHECK(lseek(fd, 0, SEEK_END) == RW_SIZE, "SEEK_END mismatch");
    CHECK(lseek(fd, 5, SEEK_SET) == 5, "SEEK_SET mismatch");
    CHECK(lseek(fd, 7, SEEK_CUR) == 12, "SEEK_CUR mismatch");
    close_ok(fd);

    fd = open(RW_PATH, O_RDONLY);
    CHECK(fd >= 0, "reopen %s ret=%d", RW_PATH, fd);
    memset(got, 0, sizeof(got));
    read_at_full(fd, SCORE_BLOCK_SIZE - 16, got, 64);
    CHECK(memcmp(got, data + SCORE_BLOCK_SIZE - 16, 64) == 0, "persisted cross-block data mismatch");
    close_ok(fd);

    CHECK(unlink(RW_PATH) == 0, "unlink %s failed", RW_PATH);
    pass("fs_score_rw");
    return 0;
}
