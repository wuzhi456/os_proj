#ifndef FS_SCORE_COMMON_H
#define FS_SCORE_COMMON_H

#include "../lib/user.h"

#define SCORE_BLOCK_SIZE 4096

#define FAIL(fmt, ...)                                                                 \
    do {                                                                               \
        printf("FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);          \
        exit(1);                                                                       \
    } while (0)

#define CHECK(cond, fmt, ...)                                                          \
    do {                                                                               \
        if (!(cond))                                                                   \
            FAIL(fmt, ##__VA_ARGS__);                                                  \
    } while (0)

static inline void pass(char *name) {
    printf("PASS %s\n", name);
}

static inline void cleanup_path(char *path) {
    unlink(path);
    rmdir(path);
}

static inline void close_ok(int fd) {
    CHECK(close(fd) == 0, "close fd=%d failed", fd);
}

static inline void write_full(int fd, void *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = write(fd, (char *)buf + off, len - off);
        CHECK(n > 0, "write_full fd=%d off=%d len=%d ret=%d", fd, off, len, n);
        off += n;
    }
}

static inline void read_full(int fd, void *buf, int len) {
    int off = 0;
    while (off < len) {
        int n = read(fd, (char *)buf + off, len - off);
        CHECK(n > 0, "read_full fd=%d off=%d len=%d ret=%d", fd, off, len, n);
        off += n;
    }
}

static inline void read_at_full(int fd, int off, void *buf, int len) {
    CHECK(lseek(fd, off, SEEK_SET) == off, "seek read_at off=%d failed", off);
    read_full(fd, buf, len);
}

static inline void write_at_full(int fd, int off, void *buf, int len) {
    CHECK(lseek(fd, off, SEEK_SET) == off, "seek write_at off=%d failed", off);
    write_full(fd, buf, len);
}

static inline void fill_pattern(char *buf, int len, int seed) {
    for (int i = 0; i < len; i++) {
        buf[i] = (char)('A' + ((seed * 17 + i) % 26));
    }
}

static inline int file_size(char *path) {
    struct stat st;
    int ret = stat(path, &st);
    CHECK(ret == 0, "stat %s ret=%d", path, ret);
    return (int)st.size;
}

static inline void expect_file_bytes(char *path, char *expect, int len) {
    char buf[256];
    CHECK(len <= (int)sizeof(buf), "expect_file_bytes len too large len=%d", len);

    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open %s ret=%d", path, fd);
    memset(buf, 0, sizeof(buf));
    read_full(fd, buf, len);
    CHECK(memcmp(buf, expect, len) == 0, "file bytes mismatch path=%s", path);
    CHECK(read(fd, buf, 1) == 0, "file has unexpected trailing bytes path=%s", path);
    close_ok(fd);
}

static inline int dir_contains(char *path, char *name) {
    struct dirent dents[16];
    int fd = open(path, O_RDONLY);
    CHECK(fd >= 0, "open dir %s ret=%d", path, fd);

    for (;;) {
        int n = getdents(fd, dents, sizeof(dents));
        CHECK(n >= 0, "getdents %s ret=%d", path, n);
        if (n == 0)
            break;
        for (int off = 0; off + (int)sizeof(struct dirent) <= n; off += sizeof(struct dirent)) {
            struct dirent *de = (struct dirent *)((char *)dents + off);
            if (de->ino != 0 && strcmp(de->name, name) == 0) {
                close(fd);
                return 1;
            }
        }
    }

    close(fd);
    return 0;
}

static inline void make_numbered_path(char *out, char *prefix, int idx) {
    char digits[16];
    int n = 0;
    int x = idx;

    do {
        digits[n++] = (char)('0' + (x % 10));
        x /= 10;
    } while (x > 0);

    while (*prefix)
        *out++ = *prefix++;
    while (n > 0)
        *out++ = digits[--n];
    *out = 0;
}

static inline uint64 time_us(void) {
    struct timeval tv;
    int ret = gettimeofday(&tv);
    CHECK(ret == 0, "gettimeofday ret=%d", ret);
    return tv.sec * 1000000 + tv.usec;
}

#endif  // FS_SCORE_COMMON_H
