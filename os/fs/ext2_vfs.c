#include "ext2.h"

#include "debug.h"
#include "defs.h"
#include "fs.h"
#include "log.h"

extern void sfs_vfs_init(void);

#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_FT_FIFO    5

#define EXT2_FIFO_SIZE 512

struct ext2_fifo_state {
    spinlock_t lock;
    char data[EXT2_FIFO_SIZE];
    uint nread;
    uint nwrite;
    int readers;
    int writers;
};

static struct superblock ext2_sb;

static struct inode *ext2_vfs_iget(struct superblock *sb, uint32 ino);

static int ext2_lookup(struct inode *dir, struct dentry *dentry);
static int ext2_create(struct inode *dir, struct dentry *dentry);
static int ext2_unlink(struct inode *dir, struct dentry *dentry);
static int ext2_mkdir(struct inode *dir, struct dentry *dentry);
static int ext2_rmdir(struct inode *dir, struct dentry *dentry);
static int ext2_mkfifo(struct inode *dir, struct dentry *dentry);
static int ext2_open(struct inode *inode, struct file *file, uint32 oflags);

static struct inode_operations ext2_inode_ops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .unlink = ext2_unlink,
    .mkdir  = ext2_mkdir,
    .rmdir  = ext2_rmdir,
    .mkfifo = ext2_mkfifo,
    .open   = ext2_open,
};

static int ext2_read(struct file *file, void *__either buf, loff_t len);
static int ext2_write(struct file *file, void *__either buf, loff_t len);
static int ext2_iterate(struct file *file, void *__either buf, loff_t len);
static int ext2_fifo_read(struct file *file, void *__either buf, loff_t len);
static int ext2_fifo_write(struct file *file, void *__either buf, loff_t len);
static int ext2_fifo_close(struct file *file);

static struct file_operations ext2_regfile_ops = {
    .close = generic_file_close,
    .read  = ext2_read,
    .write = ext2_write,
};

static struct file_operations ext2_dirfile_ops = {
    .iterate = ext2_iterate,
    .close   = generic_file_close,
};

static struct file_operations ext2_fifo_ops = {
    .read  = ext2_fifo_read,
    .write = ext2_fifo_write,
    .close = ext2_fifo_close,
};

static struct sb_operations ext2_sb_ops = {
    .free_inode   = ext2_free_inode_info,
    .write_inode  = ext2_write_inode,
    .delete_inode = ext2_delete_inode,
};

void fs_mount_root(void) {
    if (ext2_probe()) {
        ext2_vfs_init();
    } else {
        sfs_vfs_init();
    }
}

void ext2_vfs_init(void) {
    infof("ext2 vfs init");

    ext2_init();

    ext2_sb.name    = "ext2";
    ext2_sb.ops     = &ext2_sb_ops;
    ext2_sb.private = NULL;
    ext2_sb.list    = NULL;
    spinlock_init(&ext2_sb.lock, "ext2 sb list lock");

    struct inode *root = ext2_vfs_iget(&ext2_sb, 2);
    if (root == NULL)
        panic("ext2: root inode missing");
    iunlock(root);
    ext2_sb.root = root;

    assert(rootfs == NULL);
    rootfs = &ext2_sb;
}

static struct inode *ext2_vfs_iget(struct superblock *sb, uint32 ino) {
    struct inode *ind = ext2_iget(sb, ino);
    if (ind == NULL)
        return NULL;
    if (ind->imode & IMODE_DIR) {
        ind->fops = &ext2_dirfile_ops;
    } else if (ind->imode & IMODE_REG) {
        ind->fops = &ext2_regfile_ops;
    } else if (ind->imode & IMODE_FIFO) {
        ind->fops = &ext2_fifo_ops;
    } else {
        ind->fops = NULL;
    }
    ind->iops = &ext2_inode_ops;
    return ind;
}

static int ext2_read(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    if (!(ind->imode & IMODE_REG))
        return -EINVAL;
    ilock(ind);
    int ret = ext2_read_data(ind, file->pos, buf, len);
    iunlock(ind);
    if (ret > 0)
        file->pos += ret;
    return ret;
}

static int ext2_write(struct file *file, void *__either buf, loff_t len) {
    struct inode *ind = file_inode(file);
    if (!(ind->imode & IMODE_REG))
        return -EINVAL;
    ilock(ind);
    int ret = ext2_write_data(ind, file->pos, buf, len);
    iunlock(ind);
    if (ret > 0)
        file->pos += ret;
    return ret;
}

static int ext2_read_dir_entry(struct inode *dir, uint32 offset, struct ext2_dir_entry *entry, char *name,
                               uint32 name_len) {
    if (name_len == 0)
        return -EINVAL;
    int ret = ext2_read_data(dir, offset, entry, sizeof(*entry));
    if (ret < (int)sizeof(*entry))
        return -EINVAL;
    if (entry->rec_len < 8)
        return -EINVAL;
    uint32 to_copy = MIN(entry->name_len, name_len - 1);
    if (to_copy > 0) {
        ret = ext2_read_data(dir, offset + sizeof(*entry), name, to_copy);
        if (ret < (int)to_copy)
            return -EINVAL;
    }
    name[to_copy] = '\0';
    return 0;
}

static int ext2_iterate(struct file *file, void *__either buf, loff_t len) {
    struct inode *dir = file_inode(file);
    if (!(dir->imode & IMODE_DIR))
        return -EINVAL;
    if (len < sizeof(struct dirent))
        return -EINVAL;

    ilock(dir);
    loff_t pos = file->pos;
    loff_t end = dir->size;
    int copied = 0;
    struct ext2_dir_entry entry;
    char namebuf[DIRENT_NAME_MAX];

    while (pos < end && copied + (int)sizeof(struct dirent) <= len) {
        if (ext2_read_dir_entry(dir, pos, &entry, namebuf, sizeof(namebuf)) != 0)
            break;
        if (entry.inode != 0) {
            struct dirent out;
            memset(&out, 0, sizeof(out));
            out.ino = entry.inode;
            strncpy(out.name, namebuf, sizeof(out.name) - 1);
            vfs_either_copy_out((char *)buf + copied, &out, sizeof(out));
            copied += sizeof(out);
        }
        pos += entry.rec_len;
    }

    file->pos = pos;
    iunlock(dir);
    return copied;
}

static int ext2_dir_lookup_ino(struct inode *dir, const char *name, uint32 *out_ino) {
    loff_t offset = 0;
    struct ext2_dir_entry entry;
    char namebuf[DIRSIZ];
    uint32 name_len = strnlen(name, DIRSIZ);
    while (offset < dir->size) {
        if (ext2_read_dir_entry(dir, offset, &entry, namebuf, sizeof(namebuf)) != 0)
            return -EINVAL;
        if (entry.inode != 0 && entry.name_len == name_len && memcmp(namebuf, name, name_len) == 0) {
            *out_ino = entry.inode;
            return 0;
        }
        offset += entry.rec_len;
    }
    return -ENOENT;
}

static int ext2_dir_add(struct inode *dir, const char *name, uint32 ino, uint8 file_type) {
    uint32 name_len = strnlen(name, DIRSIZ);
    if (name_len == 0)
        return -EINVAL;
    uint32 need = EXT2_DIR_REC_LEN(name_len);
    uint32 block_bytes = ext2_block_size();
    if (need > block_bytes)
        return -EINVAL;
    uint8 block_buf[BSIZE];
    loff_t offset = 0;

    while (offset < dir->size) {
        memset(block_buf, 0, sizeof(block_buf));
        ext2_read_data(dir, offset, block_buf, block_bytes);
        uint32 off = 0;
        while (off < block_bytes) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len == 0)
                return -EINVAL;
            uint32 actual = EXT2_DIR_REC_LEN(de->name_len);
            if (de->inode == 0 && de->rec_len >= need) {
                de->inode = ino;
                de->name_len = name_len;
                de->file_type = file_type;
                memset(de->name, 0, de->rec_len - 8);
                memmove(de->name, name, name_len);
                int ret = ext2_write_data(dir, offset, block_buf, block_bytes);
                return ret < 0 ? ret : 0;
            }
            if (de->rec_len >= actual + need) {
                uint32 new_off = off + actual;
                struct ext2_dir_entry *ne = (struct ext2_dir_entry *)(block_buf + new_off);
                ne->inode = ino;
                ne->rec_len = de->rec_len - actual;
                ne->name_len = name_len;
                ne->file_type = file_type;
                memset(ne->name, 0, ne->rec_len - 8);
                memmove(ne->name, name, name_len);
                de->rec_len = actual;
                int ret = ext2_write_data(dir, offset, block_buf, block_bytes);
                return ret < 0 ? ret : 0;
            }
            off += de->rec_len;
        }
        offset += block_bytes;
    }

    uint32 new_block_off = ROUNDUP_2N(dir->size, block_bytes);
    memset(block_buf, 0, sizeof(block_buf));
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)block_buf;
    de->inode = ino;
    de->rec_len = block_bytes;
    de->name_len = name_len;
    de->file_type = file_type;
    memset(de->name, 0, de->rec_len - 8);
    memmove(de->name, name, name_len);
    int ret = ext2_write_data(dir, new_block_off, block_buf, block_bytes);
    return ret < 0 ? ret : 0;
}

static int ext2_dir_remove(struct inode *dir, const char *name, uint32 *out_ino) {
    uint32 block_bytes = ext2_block_size();
    uint8 block_buf[BSIZE];
    loff_t offset = 0;

    while (offset < dir->size) {
        memset(block_buf, 0, sizeof(block_buf));
        ext2_read_data(dir, offset, block_buf, block_bytes);
        uint32 off = 0;
        struct ext2_dir_entry *prev = NULL;
        while (off < block_bytes) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + off);
            if (de->rec_len == 0)
                return -EINVAL;
            if (de->inode != 0) {
                uint32 name_len = strnlen(name, DIRSIZ);
                if (de->name_len == name_len && memcmp(de->name, name, name_len) == 0) {
                    uint32 found_ino = de->inode;
                    if (prev != NULL) {
                        prev->rec_len += de->rec_len;
                    } else {
                        de->inode = 0;
                    }
                    *out_ino = found_ino;
                    int ret = ext2_write_data(dir, offset, block_buf, block_bytes);
                    return ret < 0 ? ret : 0;
                }
            }
            prev = de;
            off += de->rec_len;
        }
        offset += block_bytes;
    }
    return -ENOENT;
}

static int ext2_dir_is_empty(struct inode *dir) {
    loff_t offset = 0;
    struct ext2_dir_entry entry;
    char namebuf[DIRSIZ];
    while (offset < dir->size) {
        if (ext2_read_dir_entry(dir, offset, &entry, namebuf, sizeof(namebuf)) != 0)
            return -EINVAL;
        if (entry.inode != 0 && strncmp(namebuf, ".", DIRSIZ) != 0 && strncmp(namebuf, "..", DIRSIZ) != 0) {
            return -ENOTEMPTY;
        }
        offset += entry.rec_len;
    }
    return 0;
}

static int ext2_lookup(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_dir_lookup_ino(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;
    dentry->ind = ext2_vfs_iget(dir->sb, ino);
    if (dentry->ind == NULL)
        return -ENOENT;
    return 0;
}

static int ext2_create(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_alloc_inode_with_mode(EXT2_S_IFREG, &ino, NULL);
    if (ret < 0)
        return ret;
    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_REG);
    if (ret < 0) {
        ext2_free_inode_no(ino);
        return ret;
    }
    dentry->ind = ext2_vfs_iget(dir->sb, ino);
    if (dentry->ind == NULL)
        return -ENOENT;
    return 0;
}

static int ext2_mkdir(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_alloc_inode_with_mode(EXT2_S_IFDIR, &ino, NULL);
    if (ret < 0)
        return ret;

    struct inode *new_dir = ext2_vfs_iget(dir->sb, ino);
    if (new_dir == NULL)
        return -ENOENT;
    new_dir->nlinks = 2;
    ext2_inode_info(new_dir)->dinode.i_links_count = 2;
    ext2_write_inode(new_dir);

    uint32 block_size = ext2_block_size();

    uint8 block_buf[BSIZE];
    memset(block_buf, 0, sizeof(block_buf));
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)block_buf;
    dot->inode = ino;
    dot->rec_len = EXT2_DIR_REC_LEN(1);
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)(block_buf + dot->rec_len);
    dotdot->inode = dir->ino;
    dotdot->rec_len = block_size - dot->rec_len;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    ext2_write_data(new_dir, 0, block_buf, block_size);
    iunlockput(new_dir);

    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_DIR);
    if (ret < 0) {
        struct inode *cleanup = ext2_vfs_iget(dir->sb, ino);
        if (cleanup != NULL) {
            cleanup->nlinks = 0;
            ext2_write_inode(cleanup);
            iunlockput(cleanup);
        } else {
            ext2_free_inode_no(ino);
        }
        return ret;
    }
    dir->nlinks++;
    ext2_write_inode(dir);

    dentry->ind = ext2_vfs_iget(dir->sb, ino);
    return 0;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_dir_remove(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;
    struct inode *target = ext2_vfs_iget(dir->sb, ino);
    if (target == NULL)
        return -ENOENT;
    target->nlinks--;
    ext2_write_inode(target);
    iunlockput(target);
    return 0;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry) {
    if (!(dentry->ind->imode & IMODE_DIR))
        return -EINVAL;
    ilock(dentry->ind);
    if (ext2_dir_is_empty(dentry->ind) != 0) {
        iunlock(dentry->ind);
        return -ENOTEMPTY;
    }
    iunlock(dentry->ind);

    uint32 ino = 0;
    int ret = ext2_dir_remove(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;
    ilock(dentry->ind);
    dentry->ind->nlinks = 0;
    ext2_write_inode(dentry->ind);
    iunlock(dentry->ind);

    dir->nlinks--;
    ext2_write_inode(dir);
    return 0;
}

static int ext2_mkfifo(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_alloc_inode_with_mode(EXT2_S_IFIFO, &ino, NULL);
    if (ret < 0)
        return ret;
    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_FIFO);
    if (ret < 0) {
        ext2_free_inode_no(ino);
        return ret;
    }
    dentry->ind = ext2_vfs_iget(dir->sb, ino);
    return 0;
}

static int ext2_open(struct inode *inode, struct file *file, uint32 oflags) {
    if (!(inode->imode & IMODE_FIFO))
        return 0;

    struct ext2_inode_info *info = ext2_inode_info(inode);
    if (info->fifo_state == NULL) {
        void *fifo_page_pa = kallocpage();
        if (fifo_page_pa == NULL)
            return -ENOMEM;
        info->fifo_state = (void *)PA_TO_KVA(fifo_page_pa);
        struct ext2_fifo_state *fifo = (struct ext2_fifo_state *)info->fifo_state;
        memset(fifo, 0, sizeof(*fifo));
        spinlock_init(&fifo->lock, "ext2 fifo");
    }

    struct ext2_fifo_state *fifo = (struct ext2_fifo_state *)info->fifo_state;
    int accmode = oflags & 0x3;
    acquire(&fifo->lock);
    if (accmode == O_RDONLY) {
        fifo->readers++;
        wakeup(&fifo->readers);
        while (fifo->writers == 0)
            sleep(&fifo->writers, &fifo->lock);
    } else if (accmode == O_WRONLY) {
        fifo->writers++;
        wakeup(&fifo->writers);
        while (fifo->readers == 0)
            sleep(&fifo->readers, &fifo->lock);
    } else if (accmode == O_RDWR) {
        fifo->readers++;
        fifo->writers++;
        wakeup(&fifo->readers);
        wakeup(&fifo->writers);
    }
    release(&fifo->lock);

    return 0;
}

static int ext2_fifo_read(struct file *file, void *__either addr, loff_t n) {
    struct inode *inode = file_inode(file);
    struct ext2_inode_info *info = ext2_inode_info(inode);
    struct ext2_fifo_state *fifo = (struct ext2_fifo_state *)info->fifo_state;
    if (fifo == NULL)
        return -EINVAL;

    int i = 0;
    struct proc *pr = curr_proc();
    acquire(&fifo->lock);
    while (fifo->nread == fifo->nwrite && fifo->writers > 0) {
        if (iskilled(pr)) {
            release(&fifo->lock);
            return -1;
        }
        sleep(&fifo->nread, &fifo->lock);
    }
    if (fifo->nread == fifo->nwrite && fifo->writers == 0) {
        release(&fifo->lock);
        return 0;
    }
    for (i = 0; i < n; i++) {
        if (fifo->nread == fifo->nwrite)
            break;
        char ch = fifo->data[fifo->nread++ % EXT2_FIFO_SIZE];
        if (vfs_either_copy_out((void *)((uint64)addr + i), &ch, 1) < 0) {
            break;
        }
    }
    wakeup(&fifo->nwrite);
    release(&fifo->lock);
    return i;
}

static int ext2_fifo_write(struct file *file, void *__either addr, loff_t n) {
    struct inode *inode = file_inode(file);
    struct ext2_inode_info *info = ext2_inode_info(inode);
    struct ext2_fifo_state *fifo = (struct ext2_fifo_state *)info->fifo_state;
    if (fifo == NULL)
        return -EINVAL;

    int i = 0;
    struct proc *pr = curr_proc();
    acquire(&fifo->lock);
    while (i < n) {
        if (fifo->readers == 0 || iskilled(pr)) {
            release(&fifo->lock);
            return -1;
        }
        if (fifo->nwrite == fifo->nread + EXT2_FIFO_SIZE) {
            wakeup(&fifo->nread);
            sleep(&fifo->nwrite, &fifo->lock);
        } else {
            char ch;
            if (vfs_either_copy_in((void *)((uint64)addr + i), &ch, 1) < 0)
                break;
            fifo->data[fifo->nwrite++ % EXT2_FIFO_SIZE] = ch;
            i++;
        }
    }
    wakeup(&fifo->nread);
    release(&fifo->lock);
    return i;
}

static int ext2_fifo_close(struct file *file) {
    struct inode *inode = file_inode(file);
    struct ext2_inode_info *info = ext2_inode_info(inode);
    struct ext2_fifo_state *fifo = (struct ext2_fifo_state *)info->fifo_state;
    if (fifo == NULL)
        return generic_file_close(file);

    acquire(&fifo->lock);
    if (file->mode & FMODE_WRITE) {
        fifo->writers--;
        wakeup(&fifo->nread);
    }
    if (file->mode & FMODE_READ) {
        fifo->readers--;
        wakeup(&fifo->nwrite);
    }
    int free_fifo = (fifo->readers == 0 && fifo->writers == 0);
    release(&fifo->lock);

    if (free_fifo) {
        kfreepage((void *)KVA_TO_PA(fifo));
        info->fifo_state = NULL;
    }

    return generic_file_close(file);
}
