#include "fs_score_common.h"

#define META_PATH "/score_meta"

static char payload[SCORE_BLOCK_SIZE + 17];
static char pathbuf[64];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(META_PATH);

    int fd = open(META_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", META_PATH, fd);

    struct stat st;
    CHECK(stat("/", &st) == 0, "stat root failed");
    CHECK((st.mode & ST_MODE_DIR) != 0, "root is not directory mode=%x", st.mode);

    CHECK(fstat(fd, &st) == 0, "fstat new file failed");
    CHECK((st.mode & ST_MODE_REG) != 0, "new file is not regular mode=%x", st.mode);
    CHECK(st.size == 0, "new file size is not zero size=%d", (int)st.size);
    CHECK(st.nlinks >= 1, "new file nlinks invalid nlinks=%d", st.nlinks);

    fill_pattern(payload, sizeof(payload), 7);
    write_full(fd, payload, sizeof(payload));
    CHECK(fstat(fd, &st) == 0, "fstat after write failed");
    CHECK(st.size == sizeof(payload), "size after write got=%d expected=%d", (int)st.size, (int)sizeof(payload));

    CHECK(lseek(fd, 2, SEEK_SET) == 2, "seek for non-growing overwrite failed");
    memmove(payload, "XYZ", 3);
    write_full(fd, payload, 3);
    CHECK(fstat(fd, &st) == 0, "fstat after overwrite failed");
    CHECK(st.size == sizeof(payload), "overwrite changed file size got=%d", (int)st.size);

    int sparse_off = (int)sizeof(payload) + SCORE_BLOCK_SIZE;
    CHECK(lseek(fd, sparse_off, SEEK_SET) == sparse_off, "seek for extending write failed");
    memmove(payload, "Z", 1);
    write_full(fd, payload, 1);
    CHECK(fstat(fd, &st) == 0, "fstat after extension failed");
    CHECK(st.size == sparse_off + 1, "extended size got=%d expected=%d", (int)st.size, sparse_off + 1);
    close_ok(fd);

    CHECK(stat(META_PATH, &st) == 0, "stat by path failed");
    CHECK(st.size == sparse_off + 1, "path stat size mismatch got=%d", (int)st.size);
    CHECK(unlink(META_PATH) == 0, "unlink meta file failed");
    CHECK(stat(META_PATH, &st) < 0, "stat unexpectedly succeeded after unlink");

    for (int i = 0; i < 24; i++) {
        make_numbered_path(pathbuf, "/score_meta_tmp_", i);
        unlink(pathbuf);
        fd = open(pathbuf, O_CREAT | O_RDWR | O_TRUNC);
        CHECK(fd >= 0, "recreate loop open %s ret=%d", pathbuf, fd);
        memmove(payload, "x", 1);
        write_full(fd, payload, 1);
        CHECK(fstat(fd, &st) == 0, "recreate loop fstat failed");
        CHECK((st.mode & ST_MODE_REG) != 0, "recreate loop mode invalid mode=%x", st.mode);
        CHECK(st.size == 1, "recreate loop size invalid size=%d", (int)st.size);
        close_ok(fd);
        CHECK(unlink(pathbuf) == 0, "recreate loop unlink %s failed", pathbuf);
    }

    pass("fs_score_meta");
    return 0;
}
