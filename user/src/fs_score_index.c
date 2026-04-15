#include "fs_score_common.h"

#define INDEX_PATH "/score_index"

static char block[SCORE_BLOCK_SIZE];
static char expected[SCORE_BLOCK_SIZE];

static int should_write_block(int blockno, int blocks, int sparse) {
    if (!sparse)
        return 1;
    if (blockno == 0 || blockno == 11 || blockno == 12 || blockno == 13)
        return 1;
    if (blockno == 1035 || blockno == 1036)
        return 1;
    if (blockno == 16451 || blockno == 17000 || blockno == blocks - 1)
        return 1;
    return 0;
}

static void write_blocks(int fd, int blocks, int sparse) {
    for (int i = 0; i < blocks; i++) {
        if (!should_write_block(i, blocks, sparse))
            continue;
        fill_pattern(block, sizeof(block), i);
        write_at_full(fd, i * SCORE_BLOCK_SIZE, block, sizeof(block));
    }
}

static void verify_block(int fd, int blockno) {
    int off = blockno * SCORE_BLOCK_SIZE;
    CHECK(lseek(fd, off, SEEK_SET) == off, "seek block %d failed", blockno);
    memset(block, 0, sizeof(block));
    read_full(fd, block, sizeof(block));
    fill_pattern(expected, sizeof(expected), blockno);
    CHECK(memcmp(block, expected, sizeof(block)) == 0, "block %d data mismatch", blockno);
}

static void verify_interesting_blocks(int fd, int blocks) {
    verify_block(fd, 0);
    verify_block(fd, 11);
    verify_block(fd, 12);
    verify_block(fd, 13);
    if (blocks > 1035)
        verify_block(fd, 1035);
    if (blocks > 1036)
        verify_block(fd, 1036);
    if (blocks > 17000)
        verify_block(fd, 17000);
    verify_block(fd, blocks - 1);
}

int main(int argc, char **argv) {
    stdout_nobuf();

    int blocks = 14;
    char *mode = "single";
    int sparse = 0;
    if (argc > 1 && strcmp(argv[1], "double") == 0) {
        blocks = 1280;
        mode = "double";
    } else if (argc > 1 && strcmp(argv[1], "triple") == 0) {
        blocks = 17920;
        mode = "triple";
        sparse = 1;
    }

    unlink(INDEX_PATH);
    int fd = open(INDEX_PATH, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "create %s ret=%d", INDEX_PATH, fd);

    write_blocks(fd, blocks, sparse);
    CHECK(file_size(INDEX_PATH) == blocks * SCORE_BLOCK_SIZE, "file size after index write mismatch");

    verify_interesting_blocks(fd, blocks);
    close_ok(fd);

    fd = open(INDEX_PATH, O_RDONLY);
    CHECK(fd >= 0, "reopen index file ret=%d", fd);
    verify_interesting_blocks(fd, blocks);
    close_ok(fd);
    CHECK(unlink(INDEX_PATH) == 0, "unlink index file failed");

    printf("PASS fs_score_index %s blocks=%d\n", mode, blocks);
    return 0;
}
