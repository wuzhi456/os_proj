#include "fs_score_common.h"

#define PATH_ROOT "/score_path"
#define PATH_A    "/score_path/a"
#define PATH_B    "/score_path/a/b"
#define PATH_FILE "/score_path/a/b/file"

static char buf[64];

static void cleanup(void) {
    unlink(PATH_FILE);
    rmdir(PATH_B);
    rmdir(PATH_A);
    rmdir(PATH_ROOT);
}

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup();

    CHECK(mkdir(PATH_ROOT) == 0, "mkdir root failed");
    CHECK(mkdir(PATH_A) == 0, "mkdir a failed");
    CHECK(mkdir(PATH_B) == 0, "mkdir b failed");

    int fd = open("/score_path//a/b/./file", O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create through dot path ret=%d", fd);
    memmove(buf, "path-ok", 7);
    write_full(fd, buf, 7);
    close_ok(fd);

    fd = open("/score_path/a/b/../b/file", O_RDONLY);
    CHECK(fd >= 0, "open through dotdot path ret=%d", fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, 7);
    CHECK(memcmp(buf, "path-ok", 7) == 0, "dotdot path data mismatch");
    close_ok(fd);

    fd = open("score_path/a/b/file", O_RDONLY);
    CHECK(fd >= 0, "open relative path ret=%d", fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, 7);
    CHECK(memcmp(buf, "path-ok", 7) == 0, "relative path data mismatch");
    close_ok(fd);

    fd = open("/score_path/a/b/", O_RDONLY);
    CHECK(fd >= 0, "open trailing slash directory ret=%d", fd);
    close_ok(fd);

    CHECK(unlink(PATH_FILE) == 0, "unlink path file failed");
    CHECK(rmdir(PATH_B) == 0, "rmdir b failed");
    CHECK(rmdir(PATH_A) == 0, "rmdir a failed");
    CHECK(rmdir(PATH_ROOT) == 0, "rmdir root failed");

    pass("fs_score_path");
    return 0;
}
