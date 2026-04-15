#include "fs_score_common.h"

#define DIRENTS_DIR   "/score_dirents"
#define DIRENTS_COUNT 20

static char pathbuf[64];
static char data[16];
static int seen[DIRENTS_COUNT];

static void cleanup_all(void) {
    for (int i = 0; i < DIRENTS_COUNT; i++) {
        make_numbered_path(pathbuf, "/score_dirents/e_", i);
        unlink(pathbuf);
    }
    rmdir(DIRENTS_DIR);
}

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup_all();
    CHECK(mkdir(DIRENTS_DIR) == 0, "mkdir %s failed", DIRENTS_DIR);

    for (int i = 0; i < DIRENTS_COUNT; i++) {
        make_numbered_path(pathbuf, "/score_dirents/e_", i);
        int fd = open(pathbuf, O_CREAT | O_RDWR | O_TRUNC);
        CHECK(fd >= 0, "create %s ret=%d", pathbuf, fd);
        fill_pattern(data, sizeof(data), i);
        write_full(fd, data, sizeof(data));
        close_ok(fd);
    }

    memset(seen, 0, sizeof(seen));
    int found = 0;
    int fd = open(DIRENTS_DIR, O_RDONLY);
    CHECK(fd >= 0, "open %s ret=%d", DIRENTS_DIR, fd);

    struct dirent dent;
    for (;;) {
        int n = getdents(fd, &dent, sizeof(dent));
        CHECK(n >= 0, "getdents %s ret=%d", DIRENTS_DIR, n);
        if (n == 0)
            break;
        CHECK(n == sizeof(dent), "single-entry getdents returned n=%d", n);
        if (dent.ino == 0)
            continue;
        if (strncmp(dent.name, "e_", 2) == 0) {
            int idx = atoi(dent.name + 2);
            CHECK(idx >= 0 && idx < DIRENTS_COUNT, "dirent index out of range name=%s", dent.name);
            CHECK(seen[idx] == 0, "duplicate dirent name=%s", dent.name);
            seen[idx] = 1;
            found++;
        }
    }
    close_ok(fd);

    CHECK(found == DIRENTS_COUNT, "dirent count mismatch found=%d expected=%d", found, DIRENTS_COUNT);

    cleanup_all();
    pass("fs_score_dirents");
    return 0;
}
