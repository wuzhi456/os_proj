#include "fs_score_common.h"

#define SEQ_PATH "/bench_seq"

static char block[SCORE_BLOCK_SIZE];

int main(int argc, char **argv) {
    stdout_nobuf();

    int kib = 2048;
    if (argc > 1)
        kib = atoi(argv[1]);
    CHECK(kib >= 4, "size must be at least 4 KiB");

    int total = kib * 1024;
    unlink(SEQ_PATH);
    int fd = open(SEQ_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "open %s ret=%d", SEQ_PATH, fd);

    uint64 t0 = time_us();
    for (int off = 0, chunk = 0; off < total; off += SCORE_BLOCK_SIZE, chunk++) {
        int n = total - off;
        if (n > SCORE_BLOCK_SIZE)
            n = SCORE_BLOCK_SIZE;
        fill_pattern(block, n, chunk);
        write_full(fd, block, n);
    }
    uint64 t1 = time_us();

    CHECK(lseek(fd, 0, SEEK_SET) == 0, "seek bench seq read failed");
    for (int off = 0; off < total; off += SCORE_BLOCK_SIZE) {
        int n = total - off;
        if (n > SCORE_BLOCK_SIZE)
            n = SCORE_BLOCK_SIZE;
        read_full(fd, block, n);
    }
    uint64 t2 = time_us();

    CHECK(close(fd) == 0, "close seq bench failed");
    unlink(SEQ_PATH);

    printf("BENCH fs_bench_seq bytes=%d write_us=%d read_us=%d\n", total, (int)(t1 - t0), (int)(t2 - t1));
    return 0;
}
