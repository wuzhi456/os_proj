#include "buf.h"
#include "debug.h"
#include "defs.h"
#include "fs.h"
#include "kalloc.h"
#include "log.h"
#include "virtio.h"

static allocator_t allocator_file;
static allocator_t allocator_inode;
struct superblock* rootfs;
spinlock_t freflock;

__attribute__((weak)) void fs_mount_root(void) {
    extern void sfs_vfs_init(void);
    sfs_vfs_init();
}

void fs_init() {
    static int fs_inited = 0;
    if (fs_inited)
        return;
    fs_inited = 1;

    infof("fs_init");

    allocator_init(&allocator_file, "file", sizeof(struct file), 1024);
    allocator_init(&allocator_inode, "inode", sizeof(struct inode), 1024);
    spinlock_init(&freflock, "freflock");

    bio_init();

    infof("fs_init ends");
}

void fs_mount_root_once() {
    if (rootfs == NULL)
        fs_mount_root();

    struct proc* p = curr_proc();
    if (p != NULL && p->cwd == NULL && rootfs != NULL) {
        iget(rootfs->root);
        p->cwd = rootfs->root;
    }
}

/**
 * @brief Allocate a file structure.
 *
 * caller must initialize the returned file's fields.
 *
 * @return struct file*
 */
struct file* filealloc(void) {
    struct file* f = kalloc(&allocator_file);

    sleeplock_init(&f->lock, "filelock");
    f->ref = 1;
    f->pos = 0;
    // do not touch another fields

    return f;
}
/**
 * @brief increase ref count for file f.
 *
 * @param f
 */
void fget(struct file* f) {
    assert(f != NULL);

    acquire(&freflock);
    f->ref++;
    release(&freflock);
}

/**
 * @brief decrease ref count for file f. If refcount drops to zero, free the file.
 *
 * @param f
 */
void fput(struct file* f) {
    assert(f != NULL);

    acquire(&freflock);
    if (--f->ref > 0) {
        release(&freflock);
        return;
    }
    release(&freflock);

    if (f->ops->close)
        f->ops->close(f);  // ops->close is called with f->lock held
    kfree(&allocator_file, f);
}

void flock(struct file* f) {
    assert(f != NULL);
    acquiresleep(&f->lock);
}

void funlock(struct file* f) {
    assert(f != NULL);
    assert(f->ref > 0);
    releasesleep(&f->lock);
}

// General inode manipulation functions

/**
 * @brief Create a VFS inode, and lock it.
 * Caller must initialize the inode's fields.
 *
 * @param sb
 * @param ino
 * @return struct inode*
 */
struct inode* iget_locked(struct superblock* sb, uint32 ino) {
    // find the inode in the sb's list first
    acquire(&sb->lock);
    struct inode* inode = sb->list;
    while (inode) {
        if (inode->ino == ino) {
            inode->ref++;
            release(&sb->lock);
            debugf("ino %d ref %d", inode->ino, inode->ref);
            acquiresleep(&inode->lock);
            return inode;
        }
        inode = inode->next;
    }
    release(&sb->lock);

    inode       = kalloc(&allocator_inode);
    memset(inode, 0, sizeof(*inode));
    inode->sb   = sb;
    inode->ino  = ino;
    inode->next = NULL;
    sleeplock_init(&inode->lock, "inode");

    // insert the inode into the sb's list
    acquire(&sb->lock);
    inode->ref  = 1;
    inode->next = sb->list;
    sb->list    = inode;
    release(&sb->lock);

    debugf("ino %d ref %d", inode->ino, inode->ref);

    acquiresleep(&inode->lock);

    return inode;
}

/**
 * @brief increase ref count for inode.
 */
void iget(struct inode* inode) {
    assert(inode != NULL);
    acquire(&inode->sb->lock);
    inode->ref++;
    release(&inode->sb->lock);
    debugf("ino %d ref %d", inode->ino, inode->ref);
}

void iput(struct inode* inode) {
    assert(inode != NULL);

    acquire(&inode->sb->lock);
    if (inode->ref == 1 && inode->nlinks == 0) {
        debugf("ino %d ref %d and nlink 0, delete_inode", inode->ino, inode->ref);
        // inode has no links and no other references: delete it on disk.

        // If ref == 1, no other process can "see" a pointer to this inode.
        // Thus this acquiresleep won't block.
        acquiresleep(&inode->lock);

        // RACE CONDITION WARNING:
        // release the linked-list lock, but we still keep the inode in the list (!!!)

        // If another process tries to `iget_locked` with the same ino, it will find this inode object in the sb's list.
        //    Although the inode object is still alive and valid, it is actually a to-be-deleted inode.
        //    Soon the on-disk inode is deleted, and the in-memory inode is invalid and a phantum, causing a race condition.

        // However, we will prove that it's impossible that another process will `iget_locked` _with the same ino_.
        
        // 1. nlinks == 0 means no parent directory holds a link to this inode.
        //    All accesses (open, dlookup) to this ino must go through the parent directory. The reading and the removal of this ino from its parent directory are protected by the parent-inode's sleeplock.
        // 2. The underlying fs-implementation must not make the ino available to `ialloc`, aka doesn't free it in the bitmap.

        release(&inode->sb->lock);

        if (inode->sb->ops->delete_inode) {
            inode->sb->ops->delete_inode(inode);
        }

        releasesleep(&inode->lock);

        acquire(&inode->sb->lock);
        assert(inode->ref == 1);
    }

    inode->ref--;
    debugf("ino %d ref %d", inode->ino, inode->ref);
    if (inode->ref > 0) {
        release(&inode->sb->lock);
        return;
    }

    // remove the inode from the sb's list
    struct inode* cur = inode->sb->list;
    if (cur == inode) {
        inode->sb->list = inode->next;
    } else {
        while (cur) {
            if (cur->next == inode) {
                cur->next = inode->next;
                break;
            }
            cur = cur->next;
        }
    }
    release(&inode->sb->lock);
    if (inode->sb->ops->free_inode)
        inode->sb->ops->free_inode(inode);
    kfree(&allocator_inode, inode);
}

void imarkdirty(struct inode* inode) {
    assert(inode != NULL);
    assert(holdingsleep(&inode->lock));
    inode->sb->ops->write_inode(inode);
}

void ilock(struct inode* inode) {
    debugf("ino %d ref %d", inode->ino, inode->ref);
    assert(inode != NULL);
    assert(inode->ref > 0);
    acquiresleep(&inode->lock);
}

void iunlock(struct inode* inode) {
    debugf("ino %d ref %d", inode->ino, inode->ref);
    assert(inode != NULL);
    assert(inode->ref > 0);
    releasesleep(&inode->lock);
}

void iunlockput(struct inode *inode) {
    iunlock(inode);
    iput(inode);
}

int vfs_either_copy_out(void* __either dst, void* __kva src, loff_t len) {
    if (IS_USER_VA(dst)) {
        int ret;
        struct proc* pr = curr_proc();
        acquire(&pr->mm->lock);
        ret = copy_to_user(pr->mm, (uint64)dst, src, len);
        release(&pr->mm->lock);
        if (ret == 0)
            ret = len;
        else
            ret = -1;
        return ret;
    } else {
        memmove(dst, src, len);
        return len;
    }
}

int vfs_either_copy_in(void* __either src, void* __kva dst, loff_t len) {
    if (IS_USER_VA(src)) {
        int ret;
        struct proc* pr = curr_proc();
        acquire(&pr->mm->lock);
        ret = copy_from_user(pr->mm, dst, (uint64)src, len);
        release(&pr->mm->lock);
        if (ret == 0)
            ret = len;
        else
            ret = -1;
        return ret;
    } else {
        memmove(dst, src, len);
        return len;
    }
}

int generic_file_close(struct file* f) {
    iput(file_inode(f));
    f->private = NULL;
    return 0;
}
