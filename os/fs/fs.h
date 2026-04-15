#ifndef FS_H
#define FS_H

#include "lock.h"
#include "types.h"
#include "vm.h"
#include "fs/uapi.h"

extern struct superblock *rootfs;

#define DIRSIZ 32   // max length of a directory entry name
#define __either

typedef uint32 fmode_t;
struct file_operations;
struct inode;
// struct file is an "object" in the kernel, which can be operated with "ops".
struct file {
    // freflock proctects the ref.
    int ref;           // reference count

    sleeplock_t lock;  // lock protects pos.
    loff_t pos;        // read/write position

    // --- following fields are immutable since filealloc ---
    fmode_t mode;  // file mode
    struct file_operations *ops;
    void *private;
};

/**
 * @brief read & write is for regular files. iterate is for directories.
 *
 * file and its inode is locked by the caller
 *  , and should NOT be unlocked by the callee.
 */
struct file_operations {
    int (*read)(struct file *file, void *__either buf, loff_t len);
    int (*write)(struct file *file, void *__either buf, loff_t len);
    int (*iterate)(struct file *file, void *__either buf, loff_t len);
    int (*close)(struct file *file);
};

#define FMODE_READ  0x1
#define FMODE_WRITE 0x2

typedef uint32 imode_t;
struct inode_operations;
struct superblock;
// struct inode is a metadata of a file.
struct inode {
    struct superblock *sb;
    uint32 ino;  // inode number
    imode_t imode;
    struct file_operations *fops;
    struct inode_operations *iops;
    void *private;  // private data for filesystem implementation

    // --- above fields are immutable since allocation `iget` ---

    // sb->lock protects the next pointer and refcnt.
    int ref;             // reference count
    struct inode *next;  // for linked list in itable

    sleeplock_t lock;
    // sleeplock protects following fields
    loff_t size;  // file size
    uint16 nlinks;
};

struct dentry;
/**
 * @brief In inode_operations, dir is the parent directory.
 *
 * dir is locked by caller, and should NOT be unlocked by the callee.
 * if dentry->ind is not NULL, it is locked by the callee.
 */
struct inode_operations {
    int (*lookup)(struct inode *dir, struct dentry *dentry);
    int (*create)(struct inode *dir, struct dentry *dentry);
    int (*unlink)(struct inode *dir, struct dentry *dentry);
    int (*mkdir)(struct inode *dir, struct dentry *dentry);
    int (*rmdir)(struct inode *dir, struct dentry *dentry);
    int (*mkfifo)(struct inode *dir, struct dentry *dentry);
};

#define IMODE_DEVICE 0x100
#define IMODE_REG    0x200
#define IMODE_DIR    0x400
#define IMODE_FIFO   0x800

// struct dentry is a directory entry, which is a indirection layer between vfs ans syscalls.
// we do not use dentry as a cache, its lifetime is the same as the syscall process.
struct dentry {
    char name[DIRSIZ];  // in
    struct inode *ind;  // out
};

struct sb_operations;
// struct superblock stores all important infomation about a filesystem
struct superblock {
    char *name;  // for debugging
    void *private;
    struct sb_operations *ops;
    struct inode *root;

    // For each superblock, we have a list of *active* inodes.
    // The list is protected by the lock.
    spinlock_t lock;
    struct inode *list;
};

struct sb_operations {
    /**
     * @brief free_inode is called when an in-memory inode object is freed.
     * holds the inode->lock. but we are the only one holding the inode.
     *
     * ONLY free the inode->private.
     */
    void (*free_inode)(struct inode *inode);
    /**
     * @brief When the inode is dirty, write it back to disk.
     * holds the inode->lock.
     */
    void (*write_inode)(struct inode *inode);

    /**
     * @brief Delete the inode on-disk.
     * only called when we hold the last ref to it, AND nlinks == 0.
     */
    void (*delete_inode)(struct inode *inode);
};

#define major(dev)  ((dev) >> 16 & 0xFFFF)
#define minor(dev)  ((dev) & 0xFFFF)
#define mkdev(m, n) ((uint)((m) << 16 | (n)))

static inline struct inode *file_inode(struct file *f) {
    return f->private;
}

// vfs.c
void fs_init();
void fs_mount_root_once();
struct file *filealloc(void);
void fget(struct file *f);
void fput(struct file *f);
void flock(struct file *f);
void funlock(struct file *f);

struct inode *iget_locked(struct superblock *sb, uint32 ino);
void iget(struct inode *inode);
void iput(struct inode *inode);
void imarkdirty(struct inode *inode);
void ilock(struct inode *inode);
void iunlock(struct inode *inode);
void iunlockput(struct inode *inode);

int vfs_either_copy_out(void *__either dst, void *__kva src, loff_t len);
int vfs_either_copy_in(void *__either src, void *__kva dst, loff_t len);
int generic_file_close(struct file *f);

// dentry.c
struct inode *dlookup(char *path);
struct inode *dlookup_parent(char *path, char *name);

// bio.c
void bio_init(void);
struct buf *bread(uint dev, uint blockno);
void bwrite(struct buf *b);
void brelse(struct buf *b);
void bpin(struct buf *b);
void bunpin(struct buf *b);

// pipe.c
int pipealloc(struct file **f0, struct file **f1);
int pipeclose(struct file *f);
int pipewrite(struct file *f, void *__user addr, loff_t n);
int piperead(struct file *f, void *__user addr, loff_t n);

// vfs_syscall.c
int vfs_create(struct file** out, char* name);
int vfs_open(struct file **out, char *name, uint32 oflags);
int vfs_close(struct file *f);
int vfs_read(struct file *f, void *__user buf, loff_t len);
int vfs_write(struct file *f, void *__user buf, loff_t len);
int vfs_getdents(struct file *f, void *__user buf, loff_t len);
int vfs_lseek(struct file *f, loff_t offset, int whence);
int vfs_mkdir(const char* pathname);
int vfs_rmdir(const char* pathname);
int vfs_unlink(const char* pathname);
int vfs_mkfifo(const char* pathname);

#endif
