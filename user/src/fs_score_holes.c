#include "fs_score_common.h"

#define HOLE_PATH "/score_holes"
#define HOLE_OFF  (SCORE_BLOCK_SIZE * 2 + 17)
#define HOLE_SIZE (HOLE_OFF + 1)

static char buf[HOLE_SIZE + 16];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(HOLE_PATH);

    int fd = open(HOLE_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", HOLE_PATH, fd);

    memmove(buf, "A", 1);
    write_full(fd, buf, 1);
    CHECK(lseek(fd, HOLE_OFF, SEEK_SET) == HOLE_OFF, "seek hole off failed");
    memmove(buf, "B", 1);
    write_full(fd, buf, 1);

    CHECK(file_size(HOLE_PATH) == HOLE_SIZE, "hole file size mismatch");
    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek hole read failed");
    memset(buf, 0x7f, sizeof(buf));
    read_full(fd, buf, HOLE_SIZE);
    CHECK(buf[0] == 'A', "hole first byte mismatch");
    for (int i = 1; i < HOLE_OFF; i++) {
        CHECK(buf[i] == 0, "hole byte is not zero at off=%d val=%d", i, buf[i]);
    }
    CHECK(buf[HOLE_OFF] == 'B', "hole last byte mismatch");
    CHECK(read(fd, buf, 1) == 0, "hole eof read should return 0");
    close_ok(fd);

    CHECK(unlink(HOLE_PATH) == 0, "unlink %s failed", HOLE_PATH);
    pass("fs_score_holes");
    return 0;
}
