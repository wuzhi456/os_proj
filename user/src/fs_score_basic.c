#include "fs_score_common.h"

#define BASIC_PATH "/score_basic"

static char buf[SCORE_BLOCK_SIZE];
static char tmp[SCORE_BLOCK_SIZE];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup_path(BASIC_PATH);

    int fd = open("/", O_RDONLY);
    CHECK(fd >= 0, "root directory open failed ret=%d", fd);
    close_ok(fd);

    fd = open("/hello", O_RDWR);
    CHECK(fd >= 0, "fixture /hello open failed ret=%d", fd);
    close_ok(fd);

    CHECK(open("/score_basic_missing", O_RDONLY) < 0, "missing file open unexpectedly succeeded");

    fd = open(BASIC_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", BASIC_PATH, fd);
    memmove(buf, "abcdef", 6);
    write_full(fd, buf, 6);

    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek start after create failed");
    memset(tmp, 0, sizeof(tmp));
    read_full(fd, tmp, 6);
    CHECK(memcmp(tmp, "abcdef", 6) == 0, "read after write mismatch");

    CHECK(lseek(fd, 2, SEEK_SET) == 2, "seek for overwrite failed");
    memmove(buf, "XY", 2);
    write_full(fd, buf, 2);
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek start after overwrite failed");
    memset(tmp, 0, sizeof(tmp));
    read_full(fd, tmp, 6);
    CHECK(memcmp(tmp, "abXYef", 6) == 0, "in-place overwrite mismatch");
    close_ok(fd);

    fd = open(BASIC_PATH, O_CREAT | O_RDWR);
    CHECK(fd >= 0, "open existing with O_CREAT ret=%d", fd);
    memset(tmp, 0, sizeof(tmp));
    read_full(fd, tmp, 6);
    CHECK(memcmp(tmp, "abXYef", 6) == 0, "O_CREAT without O_TRUNC changed data");
    CHECK(read(fd, tmp, 1) == 0, "read at eof should return 0");
    close_ok(fd);

    fd = open(BASIC_PATH, O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "truncate reopen ret=%d", fd);
    CHECK(file_size(BASIC_PATH) == 0, "O_TRUNC did not reset size before write");
    memmove(buf, "ok", 2);
    write_full(fd, buf, 2);
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek start after trunc failed");
    memset(tmp, 0, sizeof(tmp));
    read_full(fd, tmp, 2);
    CHECK(memcmp(tmp, "ok", 2) == 0, "truncate data mismatch");
    CHECK(read(fd, tmp, 1) == 0, "truncate left stale bytes");
    close_ok(fd);

    CHECK(unlink(BASIC_PATH) == 0, "unlink %s failed", BASIC_PATH);
    CHECK(unlink(BASIC_PATH) < 0, "second unlink unexpectedly succeeded");
    pass("fs_score_basic");
    return 0;
}
