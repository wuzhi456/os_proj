#include "fs_score_common.h"

#define NAMES_ROOT "/score_names"
#define NAMES_DIR  "/score_names/abcdefghijklmnopqrstuvwxyza"
#define NAMES_FILE "/score_names/abcdefghijklmnopqrstuvwxyza/zyxwvutsrqponmlkjihgfedcbaz"

static char buf[64];

static void cleanup(void) {
    unlink(NAMES_FILE);
    rmdir(NAMES_DIR);
    rmdir(NAMES_ROOT);
}

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup();

    CHECK(mkdir(NAMES_ROOT) == 0, "mkdir root failed");
    CHECK(mkdir(NAMES_DIR) == 0, "mkdir long dir failed");

    int fd = open(NAMES_FILE, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create long file ret=%d", fd);
    memmove(buf, "long-name-ok", 12);
    write_full(fd, buf, 12);
    close_ok(fd);

    CHECK(dir_contains(NAMES_ROOT, "abcdefghijklmnopqrstuvwxyza"), "root dir missing long child");
    CHECK(dir_contains(NAMES_DIR, "zyxwvutsrqponmlkjihgfedcbaz"), "long dir missing long file");

    fd = open("/score_names//abcdefghijklmnopqrstuvwxyza/zyxwvutsrqponmlkjihgfedcbaz", O_RDONLY);
    CHECK(fd >= 0, "open repeated-slash long path ret=%d", fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, 12);
    CHECK(memcmp(buf, "long-name-ok", 12) == 0, "long path content mismatch");
    close_ok(fd);

    fd = open("score_names/abcdefghijklmnopqrstuvwxyza/zyxwvutsrqponmlkjihgfedcbaz", O_RDONLY);
    CHECK(fd >= 0, "open relative long path ret=%d", fd);
    close_ok(fd);

    CHECK(unlink(NAMES_FILE) == 0, "unlink long file failed");
    CHECK(rmdir(NAMES_DIR) == 0, "rmdir long dir failed");
    CHECK(rmdir(NAMES_ROOT) == 0, "rmdir names root failed");

    pass("fs_score_names");
    return 0;
}
