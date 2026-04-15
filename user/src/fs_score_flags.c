#include "fs_score_common.h"

#define FLAGS_PATH "/score_flags"

static char buf[32];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(FLAGS_PATH);

    int fd = open(FLAGS_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", FLAGS_PATH, fd);
    memmove(buf, "flags", 5);
    write_full(fd, buf, 5);
    close_ok(fd);

    fd = open(FLAGS_PATH, O_RDONLY);
    CHECK(fd >= 0, "open read-only ret=%d", fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, 5);
    CHECK(memcmp(buf, "flags", 5) == 0, "read-only data mismatch");
    memmove(buf, "X", 1);
    CHECK(write(fd, buf, 1) < 0, "write to read-only fd unexpectedly succeeded");
    close_ok(fd);

    fd = open(FLAGS_PATH, O_WRONLY);
    CHECK(fd >= 0, "open write-only ret=%d", fd);
    CHECK(read(fd, buf, 1) < 0, "read from write-only fd unexpectedly succeeded");
    memmove(buf, "F", 1);
    write_full(fd, buf, 1);
    close_ok(fd);

    expect_file_bytes(FLAGS_PATH, "Flags", 5);
    CHECK(open(FLAGS_PATH, O_RDONLY | O_TRUNC) < 0, "read-only O_TRUNC unexpectedly succeeded");
    expect_file_bytes(FLAGS_PATH, "Flags", 5);

    fd = open(FLAGS_PATH, O_WRONLY | O_TRUNC);
    CHECK(fd >= 0, "write-only truncate ret=%d", fd);
    CHECK(file_size(FLAGS_PATH) == 0, "write-only truncate did not reset size");
    memmove(buf, "ok", 2);
    write_full(fd, buf, 2);
    close_ok(fd);
    expect_file_bytes(FLAGS_PATH, "ok", 2);

    CHECK(unlink(FLAGS_PATH) == 0, "unlink %s failed", FLAGS_PATH);
    pass("fs_score_flags");
    return 0;
}
