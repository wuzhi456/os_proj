#include "fs_score_common.h"

#define FIFO_PATH "/score_fifo"

static char msg[] = "named-pipe-message";
static char buf[64];

int main(int argc, char **argv) {
    stdout_nobuf();
    (void)argc;
    (void)argv;

    unlink(FIFO_PATH);
    int ret = mkfifo(FIFO_PATH);
    CHECK(ret == 0, "mkfifo %s ret=%d", FIFO_PATH, ret);
    CHECK(mkfifo(FIFO_PATH) < 0, "duplicate mkfifo unexpectedly succeeded");

    struct stat st;
    CHECK(stat(FIFO_PATH, &st) == 0, "stat fifo failed");
    CHECK((st.mode & ST_MODE_FIFO) != 0, "fifo mode invalid mode=%x", st.mode);

    int pid = fork();
    CHECK(pid >= 0, "fork reader failed pid=%d", pid);

    if (pid == 0) {
        int rfd = open(FIFO_PATH, O_RDONLY);
        if (rfd < 0) {
            printf("FAIL fs_score_fifo child open ret=%d\n", rfd);
            exit(2);
        }
        struct stat cst;
        if (fstat(rfd, &cst) != 0 || (cst.mode & ST_MODE_FIFO) == 0) {
            printf("FAIL fs_score_fifo child fstat\n");
            exit(4);
        }
        memset(buf, 0, sizeof(buf));
        int n = read(rfd, buf, sizeof(msg) - 1);
        if (n != sizeof(msg) - 1 || memcmp(buf, msg, sizeof(msg) - 1) != 0) {
            printf("FAIL fs_score_fifo child read n=%d\n", n);
            exit(3);
        }
        close(rfd);
        exit(0);
    }

    sleep(5);
    int wfd = open(FIFO_PATH, O_WRONLY);
    if (wfd < 0) {
        kill(pid);
        int ignored;
        wait(pid, &ignored);
        FAIL("open fifo writer ret=%d", wfd);
    }

    write_full(wfd, msg, sizeof(msg) - 1);
    close_ok(wfd);

    int status = -1;
    CHECK(wait(pid, &status) == pid, "wait fifo reader failed");
    CHECK(status == 0, "fifo reader exited status=%d", status);
    CHECK(unlink(FIFO_PATH) == 0, "unlink fifo failed");

    pass("fs_score_fifo");
    return 0;
}
