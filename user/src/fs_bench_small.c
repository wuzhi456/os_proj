#include "fs_score_common.h"

static char pathbuf[64];
static char data[64];

int main(int argc, char **argv) {
    stdout_nobuf();

    int count = 64;
    if (argc > 1)
        count = atoi(argv[1]);
    CHECK(count > 0, "count must be positive");

    uint64 t0 = time_us();
    for (int i = 0; i < count; i++) {
        make_numbered_path(pathbuf, "/bench_small_", i);
        unlink(pathbuf);
        int fd = open(pathbuf, O_CREAT | O_RDWR | O_TRUNC);
        CHECK(fd >= 0, "small open %s ret=%d", pathbuf, fd);
        fill_pattern(data, sizeof(data), i);
        write_full(fd, data, sizeof(data));
        CHECK(lseek(fd, 0, SEEK_SET) == 0, "small seek %s failed", pathbuf);
        memset(data, 0, sizeof(data));
        read_full(fd, data, sizeof(data));
        CHECK(close(fd) == 0, "small close failed");
    }
    uint64 t1 = time_us();

    for (int i = 0; i < count; i++) {
        make_numbered_path(pathbuf, "/bench_small_", i);
        CHECK(unlink(pathbuf) == 0, "small unlink %s failed", pathbuf);
    }
    uint64 t2 = time_us();

    printf("BENCH fs_bench_small files=%d create_rw_us=%d unlink_us=%d\n", count, (int)(t1 - t0), (int)(t2 - t1));
    return 0;
}
