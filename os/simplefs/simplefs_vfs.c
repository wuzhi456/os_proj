#include "../fs/buf.h"
#include "simplefs.h"

extern struct sfs_vfs_superblock sb;

// vfs superblock
static struct superblock sfs;

// vfs functions
struct inode* sfs_iget(struct superblock* sb, uint32 ino);
void sfs_write_inode(struct inode* ind);
static void sfs_delete_inode(struct inode* inode);

// vfs operations
static struct sb_operations sfs_ops = {
    .free_inode   = NULL,  // no need to free inode, as we don't allocate memory for the private pointer
    .write_inode  = sfs_write_inode,
    .delete_inode = sfs_delete_inode,
};

int sfs_lookup(struct inode* parent, struct dentry* dentry);
int sfs_create(struct inode* parent, struct dentry* dentry);
int sfs_unlink(struct inode* parent, struct dentry* dentry);
int sfs_mkdir(struct inode* parent, struct dentry* dentry);
int sfs_rmdir(struct inode* parent, struct dentry* dentry);

static struct inode_operations sfs_inode_ops = {
    .lookup = sfs_lookup,
    .create = sfs_create,
    .unlink = sfs_unlink,
    .mkdir  = sfs_mkdir,
    .rmdir  = sfs_rmdir,
};

int sfs_read(struct file* file, void* __either buf, loff_t len);
int sfs_write(struct file* file, void* __either buf, loff_t len);

static struct file_operations sfs_regfile_ops = {
    .close = generic_file_close,
    .read = sfs_read,
    .write = sfs_write,
};

int sfs_iterate(struct file* file, void* __either buf, loff_t len);
static struct file_operations sfs_dirfile_ops = {
    .iterate = sfs_iterate,
};

void sfs_vfs_init(void) {
    infof("sfs vfs init");

    // read the superblock from disk
    sfs_init();

    // initialize the VFS superblock
    sfs.name    = "simplefs";
    sfs.ops     = &sfs_ops;
    sfs.private = &sb;
    sfs.list    = NULL;
    spinlock_init(&sfs.lock, "sfs sb list lock");

    struct inode* root = sfs_iget(&sfs, 0);
    iunlock(root);
    sfs.root = root;

    assert(rootfs == NULL);
    rootfs = &sfs;
}

struct inode* sfs_iget(struct superblock* sb, uint32 ino) {
    struct sfs_vfs_superblock* sfs_sb = sb->private;
    if (ino >= sfs_sb->dsb->ninodes) {
        return NULL;  // inode number out of range
    }

    struct inode* ind = iget_locked(sb, ino);

    struct buf* inode_buf         = bread(0, inode_to_disk_blkno(sfs_sb->dsb, ino));
    struct sfs_dinode* disk_inode = blk_to_inode(inode_buf->data, ino);

    ind->imode = disk_inode->type;
    if (ind->imode & IMODE_DIR) {
        ind->fops = &sfs_dirfile_ops;
    } else if (ind->imode & IMODE_REG) {
        ind->fops = &sfs_regfile_ops;
    } else {
        panic("sfs_iget: unknown imode");
    }
    ind->iops    = &sfs_inode_ops;
    ind->private = NULL;
    ind->size    = disk_inode->size;
    ind->nlinks  = disk_inode->nlink;

    brelse(inode_buf);

    return ind;
}

void sfs_write_inode(struct inode* ind) {
    iupdate(ind);
}

static int sfs_icreate(struct inode* parent, const char* name, imode_t type) {
    uint32 ino = ialloc();
    if (ino == 0) {
        return -ENOSPC;  // no space left on device
    }
    int ret = 0;

    struct buf* inode_buf         = bread(0, inode_to_disk_blkno(sb.dsb, ino));
    struct sfs_dinode* disk_inode = blk_to_inode(inode_buf->data, ino);
    assert(disk_inode->type == 0);  // should be zeroed
    disk_inode->type  = type;
    disk_inode->devno = 0;
    disk_inode->nlink = 1;  // new inode has one link
    disk_inode->size  = 0;  // new file, size is 0

    // write back the new inode
    bwrite(inode_buf);
    brelse(inode_buf);

    // add `dentry.name, ino` to the parent directory.
    struct sfs_dirent dirent;
    memset(&dirent, 0, sizeof(dirent));
    dirent.ino = ino;
    strncpy(dirent.name, name, SIMPLEFS_DIRSIZE - 1);

    ret = iwrite(parent, parent->size, &dirent, sizeof(dirent));
    if (ret < 0)
        goto err;
    // iwrite append will update parent->size automatically

    return ino;

err:
    ifree(ino);
    return ret;
}

int sfs_create(struct inode* parent, struct dentry* dentry) {
    int ret = sfs_icreate(parent, dentry->name, IMODE_REG);
    if (ret < 0)
        return ret;
    dentry->ind = sfs_iget(parent->sb, ret);
    assert(dentry->ind);
    return 0;
}

int sfs_mkdir(struct inode* parent, struct dentry* dentry) {
    int ret = sfs_icreate(parent, dentry->name, IMODE_DIR);
    if (ret < 0)
        return ret;  // failed to create the directory inode

    // create the "." and ".." entries in the new directory
    struct inode* new_dir = sfs_iget(parent->sb, ret);
    assert(new_dir);

    struct sfs_dirent dot[2];
    memset(dot, 0, sizeof(dot));
    dot[0].ino = new_dir->ino;  // "." points to itself
    dot[1].ino = parent->ino;   // ".." points to the parent directory
    strncpy(dot[0].name, ".", SIMPLEFS_DIRSIZE - 1);
    strncpy(dot[1].name, "..", SIMPLEFS_DIRSIZE - 1);

    // write these two dirents into the new directory
    ret = iwrite(new_dir, 0, dot, sizeof(dot));
    iunlockput(new_dir);
    if (ret < 0) {
        return ret;
    }

    parent->nlinks++;  // .. holds a link to the parent directory
    iupdate(parent);

    return 0;
}

static int sfs_unlink_parent(struct inode* parent, const char* name, uint32* ino) {
    int ret;
    uint32 ino_delete = 0;  // the child inode to be deleted
    uint32 parent_idx = 0;  // the index of de inside the parent directory
    struct sfs_dirent de;

    for (; parent_idx < parent->size / sizeof(struct sfs_dirent); parent_idx++) {
        ret = iread(parent, parent_idx * sizeof(de), &de, sizeof(de));
        if (ret < 0)
            return ret;

        if (strncmp(de.name, name, SIMPLEFS_DIRSIZE) == 0) {
            // found the entry to unlink
            ino_delete = de.ino;
            break;
        }
    }

    if (ino_delete == 0)
        return -ENOENT;  // entry not found

    // rewrite the directory entry with the last entry
    ret = iread(parent, (parent->size / sizeof(de) - 1) * sizeof(de), &de, sizeof(de));
    if (ret < 0)
        return ret;
    ret = iwrite(parent, parent_idx * sizeof(struct sfs_dirent), &de, sizeof(de));
    if (ret < 0)
        return ret;
    parent->size -= sizeof(de);
    iupdate(parent);  // mark the parent directory as dirty

    *ino = ino_delete;  // return the inode number of the deleted entry
    return 0;
}

static void sfs_delete_inode(struct inode* inode) {
    assert(inode->nlinks == 0);

    if (inode->imode & IMODE_DIR) {
        assert(inode->size == 2 * sizeof(struct sfs_dirent));  // should be empty
        struct sfs_dirent dot[2];

        int ret = iread(inode, 0, dot, sizeof(dot));
        if (ret < 0)
            panic("sfs_delete_inode: failed to read dot entries");

        assert(dot[0].ino == inode->ino);
        assert(strncmp(dot[1].name, "..", SIMPLEFS_DIRSIZE) == 0);

        // drop the nlinks in parent
        struct inode* parent = sfs_iget(inode->sb, dot[1].ino);
        assert(parent);
        parent->nlinks--;
        iupdate(parent);
        iunlockput(parent);
    }

    // drop all data blocks
    struct buf* inode_buf         = bread(0, inode_to_disk_blkno(sb.dsb, inode->ino));
    struct sfs_dinode* disk_inode = blk_to_inode(inode_buf->data, inode->ino);
    for (int i = 0; i < NDIRECT; i++) {
        if (disk_inode->direct[i] != 0) {
            bfree(disk_inode->direct[i]);  // free the direct block
            disk_inode->direct[i] = 0;     // clear the pointer
        }
    }
    if (disk_inode->indirect != 0) {
        struct buf* indirect_buf = bread(0, datablock_to_disk_blkno(sb.dsb, disk_inode->indirect));
        for (int i = 0; i < BSIZE / sizeof(uint32); i++) {
            uint32 blkno = ((uint32*)indirect_buf->data)[i];
            if (blkno != 0) {
                bfree(blkno);  // free the indirect block
            }
        }
        brelse(indirect_buf);
        bfree(disk_inode->indirect);  // free the indirect block itself
        disk_inode->indirect = 0;     // clear the pointer
    }
    brelse(inode_buf);  // release the inode block

    izero(inode->ino);  // zero the inode on disk
    ifree(inode->ino);  // free the inode number
}

int sfs_unlink(struct inode* parent, struct dentry* dentry) {
    uint32 ino;
    int ret = sfs_unlink_parent(parent, dentry->name, &ino);
    if (ret < 0)
        return ret;  // failed to unlink the entry

    struct inode* ind = sfs_iget(parent->sb, ino);
    assert(ind);
    ind->nlinks--;
    iupdate(ind);
    iunlockput(ind);

    return 0;
}

int sfs_rmdir(struct inode* parent, struct dentry* dentry) {
    if (dentry->ind->size != 2 * sizeof(struct sfs_dirent)) {
        return -ENOTEMPTY;  // directory is not empty, cannot remove
    }

    uint32 ino;
    int ret = sfs_unlink_parent(parent, dentry->name, &ino);
    if (ret < 0)
        return ret;  // failed to unlink the entry

    struct inode* ind = sfs_iget(parent->sb, ino);
    assert(ind);
    ind->nlinks--;
    iupdate(ind);
    iunlockput(ind);

    return 0;
}

int sfs_lookup(struct inode* parent, struct dentry* dentry) {
    int ret;
    uint32 parent_idx = 0;  // the index of de inside the parent directory
    struct sfs_dirent de;

    for (; parent_idx < parent->size / sizeof(struct sfs_dirent); parent_idx++) {
        ret = iread(parent, parent_idx * sizeof(de), &de, sizeof(de));
        if (ret < 0)
            return ret;

        if (strncmp(de.name, dentry->name, SIMPLEFS_DIRSIZE) == 0) {
            // found the entry
            dentry->ind = sfs_iget(parent->sb, de.ino);
            assert(dentry->ind);
            // we found the inode, return success
            return 0;
        }
    }
    return -ENOENT;
}

int sfs_read(struct file* file, void* __either buf, loff_t len) {
    struct inode* ind = file_inode(file);
    assert(ind);

    if (!(ind->imode & IMODE_REG)) {
        return -EINVAL;  // not a regular file
    }

    ilock(ind);
    int ret = iread(ind, file->pos, buf, len);
    iunlock(ind);

    if (ret < 0) {
        return ret;
    }

    file->pos += ret;  // update the file position
    return ret;        // return the number of bytes read
}

int sfs_write(struct file* file, void* __either buf, loff_t len) {
    struct inode* ind = file_inode(file);
    assert(ind);

    if (!(ind->imode & IMODE_REG)) {
        return -EINVAL;  // not a regular file
    }

    ilock(ind);
    int ret = iwrite(ind, file->pos, buf, len);
    iunlock(ind);

    if (ret < 0)
        return ret;

    file->pos += ret;  // update the file position
    return ret;  // return the number of bytes written
}

int sfs_iterate(struct file* file, void* __either buf, loff_t len) {
    struct inode* ind = file_inode(file);
    assert(ind);

    if (!(ind->imode & IMODE_DIR)) {
        return -EINVAL;  // not a directory
    }

    ilock(ind);
    int ret = iread(ind, file->pos, buf, len);
    iunlock(ind);

    if (ret < 0) {
        return ret;
    }

    file->pos += ret;  // update the file position
    return ret;        // return the number of bytes read
}
