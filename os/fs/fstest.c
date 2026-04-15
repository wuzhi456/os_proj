#include "defs.h"
#include "../fs/fs.h"
#include "../simplefs/simplefs.h"

extern void sfs_vfs_init();

char largebuf[PGSIZE * 4];

void fstest(uint64 unused) {
    (void)unused;
    infof("fstest");
    char buf[512];
    int ret;

    sfs_vfs_init();

    struct file *f, *f2;

    // test. 1: open and close a file
    assert_eq(0, vfs_open(&f, "/hello", O_RDWR));
    assert_eq(0, vfs_close(f));
    infof("test1 passed");
    
    // test. 2: open a file, read nothing, write 12 bytes
    assert_eq(0, vfs_open(&f, "/hello", O_RDWR));
    infof("hello inode: %d, ref: %d", file_inode(f)->ino, file_inode(f)->ref);
    assert_eq(0, vfs_read(f, buf, 512));
    strncpy(buf, "hello world", 12);
    assert_eq(12, vfs_write(f, buf, 12));
    assert_eq(0, vfs_close(f));
    infof("test2 passed");
    
    // test. 3: open a file, read 12 bytes, seek to the beginning, read again
    assert_eq(0, vfs_open(&f, "/hello", O_RDWR));
    assert_eq(12, vfs_read(f, buf, 512));
    assert_eq(strncmp(buf, "hello world", 12), 0);
    assert_eq(0, vfs_lseek(f, 0, SEEK_SET));
    assert_eq(12, vfs_read(f, buf, 512));
    assert(strncmp(buf, "hello world", 12) == 0);
    assert_eq(0, vfs_close(f));
    infof("test3 passed");

    // test. 4: open a file twice, they share the same inode
    assert_eq(0, vfs_open(&f, "/hello", O_RDWR));
    assert_eq(0, vfs_open(&f2, "/hello", O_RDWR));
    assert_eq(file_inode(f), file_inode(f2));
    assert_eq(2, file_inode(f)->ref);
    assert_eq(0, vfs_close(f));
    assert_eq(1, file_inode(f2)->ref);
    assert_eq(0, vfs_close(f2));
    infof("test4 passed");

    // test. 5: create a file
    assert_eq(0, vfs_create(&f, "/hello2"));
    assert_eq(1, file_inode(f)->ref);
    assert_eq(0, vfs_close(f));
    assert_eq(0, vfs_open(&f, "/hello2", O_RDWR));
    assert_eq(20, vfs_lseek(f, 20, SEEK_END));
    assert_eq(20, f->pos);
    assert_eq(12, vfs_write(f, buf, 12));
    assert_eq(32, f->pos);
    assert_eq(0, vfs_close(f));

    assert_eq(0, vfs_open(&f, "/hello2", O_RDWR));
    assert_eq(32, file_inode(f)->size);
    assert_eq(0, vfs_close(f));
    infof("test5 passed");

    // test. 6: create a directory
    assert_eq(0, vfs_mkdir("/testdir"));
    assert_eq(0, vfs_open(&f, "/testdir", O_RDWR));
    printf("testdir inode: %d, ref: %d\n", file_inode(f)->ino, file_inode(f)->ref);
    assert_eq(file_inode(f)->imode & IMODE_DIR, IMODE_DIR);
    assert_eq(0, vfs_close(f));
    infof("test6 passed");

    // test. 7: create a file in the directory
    assert_eq(0, vfs_open(&f, "/testdir/hello3", O_RDWR | O_CREAT));
    memmove(buf, "hello in dir", 14);
    assert_eq(14, vfs_write(f, buf, 14));
    assert_eq(0, vfs_close(f));

    memset(buf, 0, sizeof(buf));
    assert_eq(0, vfs_open(&f, "/testdir/hello3", O_RDWR));
    assert_eq(14, vfs_read(f, buf, 512));
    assert(strncmp(buf, "hello in dir", 14) == 0);
    infof("test7 passed");

    // test. 8: delete the directory
    assert_eq(-ENOENT, vfs_rmdir("/asdasd"));
    assert_eq(-ENOENT, vfs_rmdir("/testdir/asd/asdasd"));
    assert_eq(-ENOTEMPTY, vfs_rmdir("/testdir"));
    assert_eq(0, vfs_unlink("/testdir/hello3"));
    assert_eq(-ENOENT, vfs_unlink("/testdir/hello3"));
    assert_eq(0, vfs_rmdir("/testdir"));
    assert_eq(-ENOENT, vfs_rmdir("/testdir"));
    infof("test8 passed");

    // test. 9 : unlink a file while opening it.
    assert_eq(0, vfs_open(&f, "/hello2", O_RDWR));
    assert_eq(0, vfs_unlink("/hello2"));
    assert_eq(-ENOENT, vfs_open(&f2, "/hello2", O_RDWR));
    assert(file_inode(f)->ref == 1);
    assert_eq(0, vfs_close(f));
    assert_eq(-ENOENT, vfs_open(&f, "/hello2", O_RDWR));
    infof("test9 passed");

    memset(largebuf, 'a', sizeof(largebuf));

    // test. 10: create a file, write to full.
    assert_eq(0, vfs_open(&f, "/largefile", O_RDWR | O_CREAT | O_TRUNC));
    while (1) {
        ret = vfs_write(f, largebuf, sizeof(largebuf) - 1);
        if (ret < 0) {
            if (ret == -EFBIG) {
                infof("test10: EFBIG reached, stopping write");
                break;
            }
            panic("test10: unexpected error %d", ret);
        }
        infof("test10: at %d, this write: %d", file_inode(f)->size, ret);
    }
    assert_eq(MAX_BLOCKS * BSIZE, file_inode(f)->size);
    assert_eq(0, vfs_close(f));
    infof("test10 passed");

    
    exit(0);
}
