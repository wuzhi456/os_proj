#include "fs_score_common.h"

#define STRESS_DIR "/score_stress"
#define STRESS_N   48
#define REUSE_N    16

static char pathbuf[64];
static char data[64];
static char got[64];

static void cleanup_all(void) {
    for (int i = 0; i < STRESS_N; i++) {
        make_numbered_path(pathbuf, "/score_stress/f_", i);
        unlink(pathbuf);
    }
    for (int i = 0; i < REUSE_N; i++) {
        make_numbered_path(pathbuf, "/score_stress/g_", i);
        unlink(pathbuf);
    }
    rmdir(STRESS_DIR);
}

static void create_one(char *prefix, int idx) {
    make_numbered_path(pathbuf, prefix, idx);
    int fd = open(pathbuf, O_CREAT | O_RDWR | O_TRUNC);
    CHECK(fd >= 0, "stress create %s ret=%d", pathbuf, fd);
    fill_pattern(data, sizeof(data), idx);
    write_full(fd, data, sizeof(data));
    close_ok(fd);
}

static void verify_one(char *prefix, int idx) {
    make_numbered_path(pathbuf, prefix, idx);
    int fd = open(pathbuf, O_RDONLY);
    CHECK(fd >= 0, "stress open %s ret=%d", pathbuf, fd);
    memset(got, 0, sizeof(got));
    read_full(fd, got, sizeof(got));
    fill_pattern(data, sizeof(data), idx);
    CHECK(memcmp(got, data, sizeof(got)) == 0, "stress data mismatch %s", pathbuf);
    close_ok(fd);
}

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    cleanup_all();
    CHECK(mkdir(STRESS_DIR) == 0, "mkdir %s failed", STRESS_DIR);

    for (int i = 0; i < STRESS_N; i++)
        create_one("/score_stress/f_", i);

    for (int i = 0; i < STRESS_N; i++) {
        make_numbered_path(pathbuf, "f_", i);
        CHECK(dir_contains(STRESS_DIR, pathbuf), "directory missing %s", pathbuf);
        verify_one("/score_stress/f_", i);
    }

    for (int i = 0; i < STRESS_N; i += 2) {
        make_numbered_path(pathbuf, "/score_stress/f_", i);
        CHECK(unlink(pathbuf) == 0, "unlink even %s failed", pathbuf);
    }

    for (int i = 0; i < STRESS_N; i++) {
        make_numbered_path(pathbuf, "/score_stress/f_", i);
        if (i % 2 == 0)
            CHECK(open(pathbuf, O_RDONLY) < 0, "deleted file still openable %s", pathbuf);
        else
            verify_one("/score_stress/f_", i);
    }

    for (int i = 0; i < REUSE_N; i++)
        create_one("/score_stress/g_", i);
    for (int i = 0; i < REUSE_N; i++)
        verify_one("/score_stress/g_", i);

    cleanup_all();
    pass("fs_score_stress");
    return 0;
}
