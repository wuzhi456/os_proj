#include "fs_score_common.h"

#define SCORE_DIR    "/score_dir"
#define SCORE_NESTED "/score_dir/nested"
#define SCORE_FILE   "/score_dir/nested/file"

static char buf[SCORE_BLOCK_SIZE];

static void cleanup(void) {
    unlink(SCORE_FILE);
    rmdir(SCORE_NESTED);
    rmdir(SCORE_DIR);
}

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup();

    CHECK(mkdir(SCORE_DIR) == 0, "mkdir %s failed", SCORE_DIR);
    CHECK(mkdir(SCORE_DIR) < 0, "duplicate mkdir %s unexpectedly succeeded", SCORE_DIR);
    CHECK(mkdir(SCORE_NESTED) == 0, "mkdir %s failed", SCORE_NESTED);

    int fd = open(SCORE_FILE, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create nested file ret=%d", fd);
    memmove(buf, "hello from nested dir", 21);
    write_full(fd, buf, 21);
    close_ok(fd);

    fd = open(SCORE_FILE, O_RDWR);
    CHECK(fd >= 0, "reopen nested file ret=%d", fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, 21);
    CHECK(memcmp(buf, "hello from nested dir", 21) == 0, "nested file content mismatch");
    close_ok(fd);

    fd = open("/score_dir//nested///file", O_RDONLY);
    CHECK(fd >= 0, "reopen with repeated slash ret=%d", fd);
    close_ok(fd);

    fd = open("score_dir/nested/file", O_RDONLY);
    CHECK(fd >= 0, "relative path reopen ret=%d", fd);
    close_ok(fd);

    CHECK(dir_contains(SCORE_DIR, "nested"), "getdents did not find nested");
    CHECK(dir_contains(SCORE_NESTED, "file"), "getdents did not find file");
    CHECK(rmdir(SCORE_DIR) < 0, "rmdir non-empty directory unexpectedly succeeded");
    CHECK(rmdir(SCORE_FILE) < 0, "rmdir regular file unexpectedly succeeded");

    CHECK(unlink(SCORE_FILE) == 0, "unlink nested file failed");
    CHECK(open(SCORE_FILE, O_RDONLY) < 0, "unlinked nested file is still openable");
    CHECK(rmdir(SCORE_NESTED) == 0, "rmdir nested failed");
    CHECK(rmdir(SCORE_DIR) == 0, "rmdir score dir failed");
    CHECK(rmdir(SCORE_DIR) < 0, "second rmdir unexpectedly succeeded");

    pass("fs_score_dir");
    return 0;
}
