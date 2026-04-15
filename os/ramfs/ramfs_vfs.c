#include "defs.h"
#include "../fs/fs.h"
#include "ramfs.h"
#include "vm.h"

// ramfs.c
void ramfs_prepare(struct ramfs_superblock* sb);

struct inode* ramfs_iget(struct superblock* sb, uint32 ino);

static struct file_operations ramfs_dirfile_ops = {
    .iterate = NULL,
};

// ramfs in-memory superblock
extern struct ramfs_superblock ramfs_sb;

// vfs superblock
static struct superblock ramfs;

// vfs operations
static struct sb_operations ramfs_ops;
static struct inode_operations ramfs_inode_ops;
static struct file_operations ramfs_regfile_ops;
static struct file_operations ramfs_dirfile_ops;

// ramfs vfs interface
void ramfs_init() {
    infof("ramfs_init");

    ramfs_prepare(&ramfs_sb);

    ramfs.name         = "ramfs";
    ramfs.ops          = &ramfs_ops;
    ramfs.private      = &ramfs_sb;
    ramfs.list = NULL;
    spinlock_init(&ramfs.lock, "sb list lock");

    struct inode* root = ramfs_iget(&ramfs, 0);
    iunlock(root);
    ramfs.root = root;

    rootfs = &ramfs;
}

struct inode* ramfs_iget(struct superblock* sb, uint32 ino) {
    struct ramfs_superblock* ramfs_sb = sb->private;

    struct ramfs_inode* rind = ramfs_inos_find(ramfs_sb, ino);
    if (rind == NULL)
        return NULL;
    
    struct inode* ind = iget_locked(sb, ino);
    ind->imode = rind->imode;
    if (ind->imode & IMODE_DIR) {
        ind->fops = &ramfs_dirfile_ops;
    } else if (ind->imode & IMODE_REG) {
        ind->fops = &ramfs_regfile_ops;
    } else {
        panic("ramfs_iget: unknown imode");
    }
    ind->iops = &ramfs_inode_ops;
    ind->private = rind;
    ind->size = rind->size;

    return ind;
}

void ramfs_free_inode(struct inode* ind) {
    // no need to free the ramfs_inode.
}

void ramfs_write_inode(struct inode* ind) {
    // no need to write the ramfs_inode.
    // but we need to update the size of the inode.
    struct ramfs_inode* rind = ind->private;
    rind->imode = ind->imode;
    rind->size = ind->size;
    rind->nlinks = ind->nlinks;
}

static struct sb_operations ramfs_ops = {
    .free_inode  = ramfs_free_inode,
    .write_inode = ramfs_write_inode,
};


int ramfs_create(struct inode *parent, struct dentry *dentry) {
    struct ramfs_superblock* sb = parent->sb->private;
    struct ramfs_inode* parentrind = parent->private;

    // create a ramfs_inode
    struct ramfs_inode* child = ramfs_create_inode(sb, parentrind, dentry->name, IMODE_REG);
    parent->size = parentrind->size;

    // fill the vfs inode
    dentry->ind = ramfs_iget(parent->sb, child->ino);

    return 0;
}

int ramfs_mkdir(struct inode *dir, struct dentry *dentry) {
    struct ramfs_superblock* sb = dir->sb->private;
    struct ramfs_inode* parent = dir->private;

    // create a ramfs_inode
    struct ramfs_inode* child = ramfs_create_inode(sb, parent, dentry->name, IMODE_DIR);
    ramfs_create_inode(sb, child, ".", IMODE_DIR);
    ramfs_create_inode(sb, parent, "..", IMODE_DIR);
    dir->nlinks++;
    dir->size = parent->size;
    imarkdirty(dir);    
    
    // fill the vfs inode
    dentry->ind = ramfs_iget(dir->sb, child->ino);

    return 0;
}

int ramfs_lookup(struct inode *dir, struct dentry *dentry) {
    struct ramfs_superblock* sb = dir->sb->private;
    struct ramfs_inode* dir_rind = dir->private;

    // find the ramfs_inode
    struct ramfs_inode* ind = NULL;
    for (int i = 0; i < dir_rind->size; i++) {
        if (strncmp(dir_rind->dirs[i].name, dentry->name, DIRSIZ) == 0) {
            ind = ramfs_inos_find(sb, dir_rind->dirs[i].ino);
            break;
        }
    }
    if (ind == NULL)
        return -ENOENT;

    // fill the vfs inode
    dentry->ind = ramfs_iget(dir->sb, ind->ino);

    return 0;
}

static struct inode_operations ramfs_inode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir  = ramfs_mkdir,
};

int ramfs_read(struct file *file, void *__either buf, loff_t len) {
    struct inode* ind = file_inode(file);
    struct ramfs_inode* rind = ind->private;
    assert(ind->imode & IMODE_REG);

    if (file->pos >= ind->size)
        return 0;

    loff_t rlen = ind->size - file->pos;
    rlen = MIN(rlen, len);

    return vfs_either_copy_out(buf, (void *)((uint64)rind->filedata + file->pos), rlen);
}

int ramfs_write(struct file *file, void *__either buf, loff_t len) {
    struct inode* ind = file_inode(file);
    struct ramfs_inode* rind = ind->private;
    assert(rind->imode & IMODE_REG);

    if (file->pos >= PGSIZE)
        return 0;

    loff_t wlen = PGSIZE - file->pos;
    wlen = MIN(wlen, len);

    int ret = vfs_either_copy_in(buf, (void *)((uint64)rind->filedata + file->pos), wlen);
    if (ret > 0) {
        file->pos += ret;
        if (rind->size < file->pos)
            ind->size = rind->size = file->pos;
    }
    return ret;
}

static struct file_operations ramfs_regfile_ops = {
    .read  = ramfs_read,
    .write = ramfs_write,
    .close = generic_file_close,
};