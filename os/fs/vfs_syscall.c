#include "defs.h"
#include "fs.h"

int vfs_create(struct file** out, char* name) {
    return vfs_open(out, name, O_CREAT | O_WRONLY | O_TRUNC);
}

int vfs_open(struct file** out, char* name, uint32 oflags) {
    *out              = NULL;
    struct inode* ind = NULL;
    uint32 accmode    = oflags & O_RDWR;

    if (accmode != O_RDONLY && accmode != O_WRONLY && accmode != O_RDWR)
        return -EINVAL;
    if ((oflags & O_TRUNC) && accmode == O_RDONLY)
        return -EINVAL;

    if (oflags & O_CREAT) {
        // create a new file
        struct dentry dentry = {.name = {0}, .ind = NULL};

        struct inode* diri = dlookup_parent(name, dentry.name);
        if (diri == NULL)
            return -ENOENT;

        if (!(diri->imode & IMODE_DIR)) {
            iunlockput(diri);
            return -EINVAL;
        }

        if (diri->iops->lookup(diri, &dentry) == 0) {
            // file exists
            ind = dentry.ind;
            assert(ind);
            if (!(ind->imode & IMODE_REG)) {
                iunlockput(ind);
                iunlockput(diri);
                return -EINVAL;
            }
        } else {
            // file does not exist, create it
            if (diri->iops->create(diri, &dentry) != 0) {
                iunlockput(diri);
                return -EINVAL;
            }
            ind = dentry.ind;
        }
        iunlockput(diri);

        // diri is out of scope, unlock and drop reference
        //  , but ind is locked
    } else {
        ind = dlookup(name);
        if (ind == NULL)
            return -ENOENT;
    }
    // ind is locked here.

    if (ind->fops == NULL) {
        iunlockput(ind);
        return -EINVAL;
    }

    if (oflags & O_TRUNC) {
        if (!(ind->imode & IMODE_REG)) {
            iunlockput(ind);
            return -EINVAL;
        }
        ind->size = 0;
        imarkdirty(ind);
    }

    iunlock(ind);
    // unlock, but we still hold a reference to ind in f->private

    struct file* f = filealloc();
    f->private     = ind;
    f->ops         = ind->fops;
    f->mode        = 0;
    if (accmode == O_RDONLY || accmode == O_RDWR)
        f->mode |= FMODE_READ;
    if (accmode == O_WRONLY || accmode == O_RDWR)
        f->mode |= FMODE_WRITE;

    *out = f;
    return 0;
}

int vfs_close(struct file* f) {
    assert(f != NULL);
    fput(f);
    return 0;
}

int vfs_read(struct file* f, void* __either buf, loff_t len) {
    int ret = -EINVAL;

    if (!(f->mode & FMODE_READ))
        return -EINVAL;

    // accessing f->ops does not require holding the lock
    //  , because f->ops is set when the file is created and never changed
    if (f->ops->read) {
        flock(f);
        ret = f->ops->read(f, buf, len);
        funlock(f);
    }

    return ret;
}

int vfs_write(struct file* f, void* __either buf, loff_t len) {
    assert(f != NULL);

    int ret = -EINVAL;

    if (!(f->mode & FMODE_WRITE))
        return -EINVAL;

    if (f->ops->write) {
        flock(f);
        ret = f->ops->write(f, buf, len);
        funlock(f);
    }

    return ret;
}

int vfs_getdents(struct file* f, void* __either buf, loff_t len) {
    assert(f != NULL);

    int ret = -EINVAL;

    if (!(f->mode & FMODE_READ))
        return -EINVAL;

    if (f->ops->iterate) {
        flock(f);
        ret = f->ops->iterate(f, buf, len);
        funlock(f);
    }

    return ret;
}

int vfs_lseek(struct file* f, loff_t offset, int whence) {
    assert(f != NULL);
    struct inode* inode = file_inode(f);
    flock(f);
    if (whence == SEEK_SET)
        f->pos = offset;
    else if (whence == SEEK_CUR)
        f->pos += offset;
    else if (whence == SEEK_END) {
        ilock(inode);
        loff_t size = inode->size;
        iunlock(inode);
        f->pos = size + offset;
    } else {
        funlock(f);
        return -EINVAL;
    }
    loff_t pos = f->pos;
    funlock(f);

    return pos;
}

int vfs_mkdir(const char* pathname) {
    char name[256];
    strncpy(name, pathname, sizeof(name) - 1);
    struct dentry dentry = {.name = {0}, .ind = NULL};
    int ret           = 0;

    struct inode* ip = dlookup_parent(name, dentry.name);
    if (ip == NULL)
        return -ENOENT;

    if (!(ip->imode & IMODE_DIR)) {
        iunlockput(ip);
        return -EINVAL;
    }

    if (ip->iops->lookup(ip, &dentry) == 0) {
        // file exists
        iunlockput(ip);
        iunlockput(dentry.ind);
        return -EEXIST;
    }

    // file does not exist, create it
    ret = ip->iops->mkdir(ip, &dentry);
    if (dentry.ind != NULL)
        iunlockput(dentry.ind);
    iunlockput(ip);
    return ret;
}

int vfs_rmdir(const char* pathname) {
    char name[256];
    strncpy(name, pathname, sizeof(name) - 1);
    struct dentry dentry = {.name = {0}, .ind = NULL};

    int ret          = 0;
    struct inode* ip = dlookup_parent(name, dentry.name);
    if (ip == NULL)
        return -ENOENT;
    if (!(ip->imode & IMODE_DIR)) {
        ret = -EINVAL;
        goto err;
    }
    if (ip->iops->lookup(ip, &dentry) != 0) {
        ret = -ENOENT;
        goto err;
    }
    if (!(dentry.ind->imode & IMODE_DIR)) {
        ret = -EINVAL;
        goto err;
    }
    iunlock(dentry.ind);
    ret = ip->iops->rmdir(ip, &dentry);
    iput(dentry.ind);
    iunlockput(ip);
    return ret;

err:
    iunlockput(ip);
    if (dentry.ind != NULL) {
        iunlockput(dentry.ind);
    }
    return ret;
}

int vfs_unlink(const char* pathname) {
    char name[256];
    strncpy(name, pathname, sizeof(name) - 1);
    int ret              = 0;
    struct dentry dentry = {.name = {0}, .ind = NULL};

    struct inode* ip = dlookup_parent(name, dentry.name);
    if (ip == NULL)
        return -ENOENT;
    if (!(ip->imode & IMODE_DIR)) {
        ret = -EINVAL;
        goto err;
    }
    if (ip->iops->lookup(ip, &dentry) != 0) {
        ret = -ENOENT;
        goto err;
    }
    if (!(dentry.ind->imode & IMODE_REG || dentry.ind->imode & IMODE_DEVICE || dentry.ind->imode & IMODE_FIFO)) {
        ret = -EINVAL;
        goto err;
    }
    iunlockput(dentry.ind);
    ret = ip->iops->unlink(ip, &dentry);
    iunlockput(ip);
    return ret;

err:
    iunlockput(ip);
    if (dentry.ind != NULL) {
        iunlockput(dentry.ind);
    }
    return ret;
}

int vfs_mkfifo(const char* pathname) {
    char name[256];
    strncpy(name, pathname, sizeof(name) - 1);
    struct dentry dentry = {.name = {0}, .ind = NULL};
    int ret              = 0;

    struct inode* ip = dlookup_parent(name, dentry.name);
    if (ip == NULL)
        return -ENOENT;

    if (!(ip->imode & IMODE_DIR)) {
        ret = -EINVAL;
        goto out;
    }

    if (ip->iops->lookup(ip, &dentry) == 0) {
        ret = -EEXIST;
        goto out;
    }

    if (ip->iops->mkfifo == NULL) {
        ret = -EINVAL;
        goto out;
    }

    ret = ip->iops->mkfifo(ip, &dentry);

out:
    iunlockput(ip);
    if (dentry.ind != NULL)
        iunlockput(dentry.ind);
    return ret;
}
