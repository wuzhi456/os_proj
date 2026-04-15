#include "fs_score_common.h"

#define RAND_PATH "/bench_rand"

static char block[SCORE_BLOCK_SIZE];

static uint next_rand(uint *seed) {
    *seed = *seed * 1664525u + 1013904223u;
    return *seed;
}

int main(int argc, char **argv) {
    stdout_nobuf();

    int blocks = 256;
    int ops = 256;
    if (argc > 1)
        blocks = atoi(argv[1]);
    if (argc > 2)
        ops = atoi(argv[2]);
    CHECK(blocks > 0 && ops > 0, "blocks and ops must be positive");

    unlink(RAND_PATH);
    int fd = open(RAND_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "open %s ret=%d", RAND_PATH, fd);

    for (int i = 0; i < blocks; i++) {
        fill_pattern(block, sizeof(block), i);
        write_full(fd, block, sizeof(block));
    }

    uint seed = 0xC0FFEEu;
    uint64 t0 = time_us();
    for (int i = 0; i < ops; i++) {
        int b = next_rand(&seed) % blocks;
        int off = b * SCORE_BLOCK_SIZE;
        CHECK(lseek(fd, off, SEEK_SET) == off, "random write seek failed");
        fill_pattern(block, sizeof(block), i);
        write_full(fd, block, sizeof(block));
    }
    uint64 t1 = time_us();

    for (int i = 0; i < ops; i++) {
        int b = next_rand(&seed) % blocks;
        int off = b * SCORE_BLOCK_SIZE;
        CHECK(lseek(fd, off, SEEK_SET) == off, "random read seek failed");
        read_full(fd, block, sizeof(block));
    }
    uint64 t2 = time_us();

    CHECK(close(fd) == 0, "close rand bench failed");
    unlink(RAND_PATH);

    printf("BENCH fs_bench_rand blocks=%d ops=%d write_us=%d read_us=%d\n", blocks, ops, (int)(t1 - t0), (int)(t2 - t1));
    return 0;
}
