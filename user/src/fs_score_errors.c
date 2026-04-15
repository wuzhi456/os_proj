#include "fs_score_common.h"

#define ERR_DIR  "/score_errors_dir"
#define ERR_FILE "/score_errors_file"

static char buf[64];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(ERR_FILE);
    rmdir(ERR_DIR);

    CHECK(open("/score_errors_missing", O_RDONLY) < 0, "missing open unexpectedly succeeded");
    CHECK(unlink("/score_errors_missing") < 0, "missing unlink unexpectedly succeeded");
    CHECK(rmdir("/score_errors_missing") < 0, "missing rmdir unexpectedly succeeded");

    struct stat st;
    CHECK(stat("/score_errors_missing", &st) < 0, "missing stat unexpectedly succeeded");

    CHECK(mkdir(ERR_DIR) == 0, "mkdir %s failed", ERR_DIR);
    CHECK(mkdir(ERR_DIR) < 0, "duplicate mkdir unexpectedly succeeded");

    int fd = open(ERR_FILE, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", ERR_FILE, fd);
    memmove(buf, "x", 1);
    write_full(fd, buf, 1);

    struct dirent dents[4];
    CHECK(getdents(fd, dents, sizeof(dents)) < 0, "getdents on regular file unexpectedly succeeded");
    CHECK(lseek(fd, 0, 99) < 0, "lseek invalid whence unexpectedly succeeded");
    close_ok(fd);

    CHECK(mkdir("/score_errors_file/child") < 0, "mkdir under regular file unexpectedly succeeded");
    CHECK(rmdir(ERR_FILE) < 0, "rmdir regular file unexpectedly succeeded");

    CHECK(unlink(ERR_FILE) == 0, "unlink %s failed", ERR_FILE);
    CHECK(rmdir(ERR_DIR) == 0, "rmdir %s failed", ERR_DIR);

    pass("fs_score_errors");
    return 0;
}
