#include "ext2/ext2.h"

#include "defs.h"
#include "simplefs/simplefs.h"

#define EXT2_FIFO_SIZE 512

struct ext2_fifo {
    spinlock_t lock;
    char data[EXT2_FIFO_SIZE];
    uint nread;
    uint nwrite;
};

static struct superblock ext2_sb;
static struct sb_operations ext2_sb_ops;
static struct inode_operations ext2_iops;
static struct file_operations ext2_reg_fops;
static struct file_operations ext2_dir_fops;
static struct file_operations ext2_fifo_fops;

static allocator_t ext2_inode_allocator;
static allocator_t ext2_fifo_allocator;

static imode_t ext2_mode_to_imode(uint16 mode) {
    uint16 type = mode & 0xF000;
    if (type == EXT2_S_IFDIR)
        return IMODE_DIR;
    if (type == EXT2_S_IFIFO)
        return IMODE_FIFO;
    return IMODE_REG;
}

static uint16 ext2_imode_to_mode(imode_t imode) {
    if (imode & IMODE_DIR)
        return EXT2_S_IFDIR | 0755;
    if (imode & IMODE_FIFO)
        return EXT2_S_IFIFO | 0644;
    return EXT2_S_IFREG | 0644;
}

static uint16 ext2_dir_entry_len(uint8 name_len) {
    return (uint16)((8 + name_len + 3) & ~3);
}

static uint32 ext2_name_len(const char *name) {
    uint32 n = 0;
    while (n < DIRSIZ && name[n])
        n++;
    return n;
}

static uint8 *ext2_tmpbuf_alloc(void **pa_out) {
    void *pa = kallocpage();
    if (pa == NULL)
        return NULL;
    *pa_out = pa;
    return (uint8 *)PA_TO_KVA(pa);
}

static void ext2_tmpbuf_free(void *pa) {
    if (pa)
        kfreepage(pa);
}

static struct ext2_fifo *ext2_fifo_get(struct inode *inode) {
    struct ext2_inode_info *info = inode->private;
    if (info->fifo != NULL)
        return (struct ext2_fifo *)info->fifo;

    int locked = 0;
    if (!holdingsleep(&inode->lock)) {
        ilock(inode);
        locked = 1;
    }
    if (info->fifo == NULL) {
        struct ext2_fifo *fifo = kalloc(&ext2_fifo_allocator);
        memset(fifo, 0, sizeof(*fifo));
        spinlock_init(&fifo->lock, "ext2 fifo");
        info->fifo = fifo;
    }
    if (locked)
        iunlock(inode);

    return (struct ext2_fifo *)info->fifo;
}

static int ext2_dir_lookup_inode(struct inode *dir, const char *name, uint32 *ino_out) {
    uint32 bs = ext2_get_fs()->block_size;
    uint32 size = dir->size;
    uint32 off = 0;
    uint32 name_len = ext2_name_len(name);
    void *pa = NULL;
    uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL)
        return -ENOMEM;

    while (off < size) {
        memset(block_buf, 0, bs);
        int n = ext2_readi(dir, off, block_buf, bs);
        if (n < 0) {
            ext2_tmpbuf_free(pa);
            return n;
        }

        uint32 boff = 0;
        while (boff + 8 <= bs) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + boff);
            if (de->rec_len == 0 || boff + de->rec_len > bs)
                break;
            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                *ino_out = de->inode;
                ext2_tmpbuf_free(pa);
                return 0;
            }
            boff += de->rec_len;
        }
        off += bs;
    }
    ext2_tmpbuf_free(pa);
    return -ENOENT;
}

static int ext2_dir_add(struct inode *dir, const char *name, uint32 ino, uint8 file_type) {
    uint32 bs = ext2_get_fs()->block_size;
    uint32 size = dir->size;
    uint32 name_len = ext2_name_len(name);
    uint16 new_len = ext2_dir_entry_len((uint8)name_len);
    void *pa = NULL;
        uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL)
        return -ENOMEM;

    for (uint32 off = 0; off < size; off += bs) {
        memset(block_buf, 0, bs);
        int n = ext2_readi(dir, off, block_buf, bs);
        if (n < 0) {
            ext2_tmpbuf_free(pa);
            return n;
        }

        uint32 boff = 0;
        while (boff + 8 <= bs) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + boff);
            if (de->rec_len == 0 || boff + de->rec_len > bs)
                break;

            uint16 used = ext2_dir_entry_len(de->name_len);
            if (de->inode == 0 && de->rec_len >= new_len) {
                de->inode = ino;
                de->name_len = (uint8)name_len;
                de->file_type = file_type;
                memmove(de->name, name, name_len);
                int ret = ext2_writei(dir, off, block_buf, bs);
                if (ret < 0) {
                    ext2_tmpbuf_free(pa);
                    return ret;
                }
                ext2_tmpbuf_free(pa);
                return 0;
            }

            if (de->rec_len > used && de->rec_len - used >= new_len) {
                struct ext2_dir_entry *newde = (struct ext2_dir_entry *)(block_buf + boff + used);
                newde->inode = ino;
                newde->rec_len = de->rec_len - used;
                newde->name_len = (uint8)name_len;
                newde->file_type = file_type;
                memmove(newde->name, name, name_len);

                de->rec_len = used;
                int ret = ext2_writei(dir, off, block_buf, bs);
                if (ret < 0) {
                    ext2_tmpbuf_free(pa);
                    return ret;
                }
                ext2_tmpbuf_free(pa);
                return 0;
            }

            boff += de->rec_len;
        }
    }

    memset(block_buf, 0, bs);
    struct ext2_dir_entry *de = (struct ext2_dir_entry *)block_buf;
    de->inode = ino;
    de->rec_len = (uint16)bs;
    de->name_len = (uint8)name_len;
    de->file_type = file_type;
    memmove(de->name, name, name_len);

        int ret = ext2_writei(dir, size, block_buf, bs);
    if (ret < 0) {
        ext2_tmpbuf_free(pa);
        return ret;
    }
    ext2_tmpbuf_free(pa);
    return 0;
}

static int ext2_dir_remove(struct inode *dir, const char *name, uint32 *ino_out) {
    uint32 bs = ext2_get_fs()->block_size;
    uint32 size = dir->size;
    uint32 name_len = ext2_name_len(name);
    void *pa = NULL;
    uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL)
        return -ENOMEM;

    for (uint32 off = 0; off < size; off += bs) {
        memset(block_buf, 0, bs);
        int n = ext2_readi(dir, off, block_buf, bs);
        if (n < 0) {
            ext2_tmpbuf_free(pa);
            return n;
        }

        uint32 boff = 0;
        struct ext2_dir_entry *prev = NULL;
        while (boff + 8 <= bs) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + boff);
            if (de->rec_len == 0 || boff + de->rec_len > bs)
                break;

            if (de->inode != 0 && de->name_len == name_len &&
                memcmp(de->name, name, name_len) == 0) {
                uint32 found = de->inode;
                if (prev) {
                    prev->rec_len += de->rec_len;
                } else {
                    de->inode = 0;
                }
                int ret = ext2_writei(dir, off, block_buf, bs);
                if (ret < 0) {
                    ext2_tmpbuf_free(pa);
                    return ret;
                }
                *ino_out = found;
                ext2_tmpbuf_free(pa);
                return 0;
            }

            prev = de;
            boff += de->rec_len;
        }
    }
    ext2_tmpbuf_free(pa);
    return -ENOENT;
}

static int ext2_dir_is_empty(struct inode *dir) {
    uint32 bs = ext2_get_fs()->block_size;
    uint32 size = dir->size;
    void *pa = NULL;
    uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL)
        return 0;

    for (uint32 off = 0; off < size; off += bs) {
        memset(block_buf, 0, bs);
        int n = ext2_readi(dir, off, block_buf, bs);
        if (n < 0) {
            ext2_tmpbuf_free(pa);
            return 0;
        }

        uint32 boff = 0;
        while (boff + 8 <= bs) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + boff);
            if (de->rec_len == 0 || boff + de->rec_len > bs)
                break;
            if (de->inode != 0) {
                if (!(de->name_len == 1 && de->name[0] == '.') &&
                    !(de->name_len == 2 && de->name[0] == '.' && de->name[1] == '.')) {
                    ext2_tmpbuf_free(pa);
                    return 0;
                }
            }
            boff += de->rec_len;
        }
    }
    ext2_tmpbuf_free(pa);
    return 1;
}

static int ext2_find_root_entry(struct inode *root, const char *name, uint32 *ino_out) {
    return ext2_dir_lookup_inode(root, name, ino_out);
}

static struct inode *ext2_iget(struct superblock *sb, uint32 ino) {
    struct inode *ind = iget_locked(sb, ino);
    if (ind->private != NULL)
        return ind;

    struct ext2_inode_info *info = kalloc(&ext2_inode_allocator);
    memset(info, 0, sizeof(*info));
    info->ino = ino;

    if (ext2_read_inode(ino, &info->inode) < 0) {
        kfree(&ext2_inode_allocator, info);
        iunlockput(ind);
        return NULL;
    }

    ind->imode = ext2_mode_to_imode(info->inode.i_mode);
    if (ind->imode & IMODE_DIR)
        ind->fops = &ext2_dir_fops;
    else if (ind->imode & IMODE_FIFO)
        ind->fops = &ext2_fifo_fops;
    else
        ind->fops = &ext2_reg_fops;

    ind->iops = &ext2_iops;
    ind->private = info;
    ind->size = info->inode.i_size;
    ind->nlinks = info->inode.i_links_count;

    return ind;
}

static void ext2_free_inode_private(struct inode *inode) {
    struct ext2_inode_info *info = inode->private;
    if (info == NULL)
        return;
    if (info->fifo) {
        kfree(&ext2_fifo_allocator, info->fifo);
        info->fifo = NULL;
    }
    kfree(&ext2_inode_allocator, info);
    inode->private = NULL;
}

static void ext2_write_inode_private(struct inode *inode) {
    struct ext2_inode_info *info = inode->private;
    if (info == NULL)
        return;

    if (inode->size == 0 && info->inode.i_size != 0)
        ext2_truncate(inode, 0);

    info->inode.i_mode = ext2_imode_to_mode(inode->imode);
    info->inode.i_size = inode->size;
    info->inode.i_links_count = inode->nlinks;
    ext2_write_inode(info->ino, &info->inode);
}

static void ext2_delete_inode(struct inode *inode) {
    struct ext2_inode_info *info = inode->private;
    if (info == NULL)
        return;

    ext2_truncate(inode, 0);
    info->inode.i_size = 0;
    info->inode.i_links_count = 0;
    ext2_write_inode(info->ino, &info->inode);
    ext2_free_inode(info->ino);
}

static int ext2_lookup(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_dir_lookup_inode(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;

    dentry->ind = ext2_iget(dir->sb, ino);
    if (dentry->ind == NULL)
        return -EINVAL;
    return 0;
}

static int ext2_create_inode(struct inode *dir, imode_t imode, uint32 *ino_out, struct ext2_inode *disk_inode) {
    (void)dir;
    uint32 ino = ext2_alloc_inode();
    if (ino == 0)
        return -ENOSPC;

    struct ext2_inode inode;
    memset(&inode, 0, sizeof(inode));
    inode.i_mode = ext2_imode_to_mode(imode);
    inode.i_links_count = (imode & IMODE_DIR) ? 2 : 1;
    inode.i_size = 0;

    if (disk_inode)
        *disk_inode = inode;
    ext2_write_inode(ino, &inode);
    *ino_out = ino;
    return 0;
}

static int ext2_create(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_create_inode(dir, IMODE_REG, &ino, NULL);
    if (ret < 0)
        return ret;

    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_REG_FILE);
    if (ret < 0) {
        ext2_free_inode(ino);
        return ret;
    }

    dentry->ind = ext2_iget(dir->sb, ino);
    return 0;
}

static int ext2_mkfifo(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_create_inode(dir, IMODE_FIFO, &ino, NULL);
    if (ret < 0)
        return ret;

    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_FIFO);
    if (ret < 0) {
        ext2_free_inode(ino);
        return ret;
    }

    dentry->ind = ext2_iget(dir->sb, ino);
    return 0;
}

static int ext2_mkdir(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    struct ext2_inode new_inode;
    int ret = ext2_create_inode(dir, IMODE_DIR, &ino, &new_inode);
    if (ret < 0)
        return ret;

    uint32 bs = ext2_get_fs()->block_size;
    uint32 blkno = ext2_alloc_block();
    if (blkno == 0) {
        ext2_free_inode(ino);
        return -ENOSPC;
    }

    new_inode.i_block[0] = blkno;
    new_inode.i_size = bs;
    new_inode.i_links_count = 2;
    new_inode.i_blocks = bs / 512;

    void *pa = NULL;
    uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL) {
        ext2_free_block(blkno);
        ext2_free_inode(ino);
        return -ENOMEM;
    }
    memset(block_buf, 0, bs);
    struct ext2_dir_entry *dot = (struct ext2_dir_entry *)block_buf;
    uint16 dot_len = ext2_dir_entry_len(1);
    dot->inode = ino;
    dot->rec_len = dot_len;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    dot->name[0] = '.';

    struct ext2_dir_entry *dotdot = (struct ext2_dir_entry *)(block_buf + dot_len);
    dotdot->inode = dir->ino;
    dotdot->rec_len = bs - dot_len;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';

    ext2_write_inode(ino, &new_inode);

    struct inode *newdir = ext2_iget(dir->sb, ino);
    if (newdir == NULL) {
        ext2_tmpbuf_free(pa);
        ext2_free_block(blkno);
        ext2_free_inode(ino);
        return -EINVAL;
    }
    int wret = ext2_writei(newdir, 0, block_buf, bs);
    iunlockput(newdir);
    ext2_tmpbuf_free(pa);
    if (wret < 0) {
        ext2_free_block(blkno);
        ext2_free_inode(ino);
        return wret;
    }

    ret = ext2_dir_add(dir, dentry->name, ino, EXT2_FT_DIR);
    if (ret < 0) {
        ext2_free_block(blkno);
        ext2_free_inode(ino);
        return ret;
    }
    dir->nlinks++;
    imarkdirty(dir);

    dentry->ind = ext2_iget(dir->sb, ino);
    return 0;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_dir_remove(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;

    struct inode *ind = ext2_iget(dir->sb, ino);
    if (ind == NULL)
        return -EINVAL;

    ind->nlinks--;
    imarkdirty(ind);
    iunlockput(ind);
    return 0;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry) {
    uint32 ino = 0;
    int ret = ext2_dir_lookup_inode(dir, dentry->name, &ino);
    if (ret < 0)
        return ret;

    struct inode *ind = ext2_iget(dir->sb, ino);
    if (ind == NULL)
        return -EINVAL;

    if (!(ind->imode & IMODE_DIR)) {
        iunlockput(ind);
        return -EINVAL;
    }

    if (!ext2_dir_is_empty(ind)) {
        iunlockput(ind);
        return -ENOTEMPTY;
    }

    ret = ext2_dir_remove(dir, dentry->name, &ino);
    if (ret < 0) {
        iunlockput(ind);
        return ret;
    }

    dir->nlinks--;
    imarkdirty(dir);

    ind->nlinks = 0;
    imarkdirty(ind);
    iunlockput(ind);
    return 0;
}

static int ext2_read(struct file *file, void *__either buf, loff_t len) {
    struct inode *inode = file_inode(file);
    if (!(inode->imode & IMODE_REG) && !(inode->imode & IMODE_FIFO))
        return -EINVAL;

    if (inode->imode & IMODE_FIFO) {
        return ext2_fifo_fops.read(file, buf, len);
    }

    ilock(inode);
    int ret = ext2_readi(inode, file->pos, buf, len);
    iunlock(inode);
    if (ret > 0)
        file->pos += ret;
    return ret;
}

static int ext2_write(struct file *file, void *__either buf, loff_t len) {
    struct inode *inode = file_inode(file);
    if (!(inode->imode & IMODE_REG) && !(inode->imode & IMODE_FIFO))
        return -EINVAL;

    if (inode->imode & IMODE_FIFO) {
        return ext2_fifo_fops.write(file, buf, len);
    }

    ilock(inode);
    int ret = ext2_writei(inode, file->pos, buf, len);
    iunlock(inode);
    if (ret > 0)
        file->pos += ret;
    return ret;
}

static int ext2_iterate(struct file *file, void *__either buf, loff_t len) {
    struct inode *inode = file_inode(file);
    if (!(inode->imode & IMODE_DIR))
        return -EINVAL;
    if (len < (loff_t)sizeof(struct dirent))
        return -EINVAL;

    uint32 bs = ext2_get_fs()->block_size;
    loff_t pos = file->pos;
    loff_t size = inode->size;
    loff_t out = 0;
    void *pa = NULL;
    uint8 *block_buf = ext2_tmpbuf_alloc(&pa);
    if (block_buf == NULL)
        return -ENOMEM;

    ilock(inode);
    while (out + (loff_t)sizeof(struct dirent) <= len && pos < size) {
        uint32 block_idx = (uint32)(pos / bs);
        uint32 block_off = (uint32)(pos % bs);

        memset(block_buf, 0, bs);
        int ret = ext2_readi(inode, block_idx * bs, block_buf, bs);
            ret = ext2_readi(inode, block_idx * bs, block_buf, bs);
        if (ret < 0) {
            iunlock(inode);
            ext2_tmpbuf_free(pa);
            return ret;
        }

        while (block_off + 8 <= bs && pos < size && out + (loff_t)sizeof(struct dirent) <= len) {
            struct ext2_dir_entry *de = (struct ext2_dir_entry *)(block_buf + block_off);
            if (de->rec_len == 0 || block_off + de->rec_len > bs) {
                pos = (block_idx + 1) * bs;
                break;
            }

            if (de->inode != 0) {
                struct dirent dent;
                memset(&dent, 0, sizeof(dent));
                dent.ino = de->inode;
                uint32 copy_len = MIN((uint32)de->name_len, (uint32)(DIRENT_NAME_MAX - 1));
                memmove(dent.name, de->name, copy_len);
                vfs_either_copy_out((void *)((uint64)buf + out), &dent, sizeof(dent));
                out += sizeof(dent);
            }

            block_off += de->rec_len;
            pos = block_idx * bs + block_off;
        }
    }

    file->pos = pos;
    iunlock(inode);
    ext2_tmpbuf_free(pa);
    return out;
}

static int ext2_fifo_read(struct file *file, void *__either buf, loff_t len) {
    struct inode *inode = file_inode(file);
    struct ext2_fifo *fifo = ext2_fifo_get(inode);
    struct proc *pr = curr_proc();

    acquire(&fifo->lock);
    while (fifo->nread == fifo->nwrite) {
        if (iskilled(pr)) {
            release(&fifo->lock);
            return -1;
        }
        sleep(&fifo->nread, &fifo->lock);
    }

    int i = 0;
    while (i < len && fifo->nread != fifo->nwrite) {
        char ch = fifo->data[fifo->nread++ % EXT2_FIFO_SIZE];
        vfs_either_copy_out((void *)((uint64)buf + i), &ch, 1);
        i++;
    }

    wakeup(&fifo->nwrite);
    release(&fifo->lock);
    return i;
}

static int ext2_fifo_write(struct file *file, void *__either buf, loff_t len) {
    struct inode *inode = file_inode(file);
    struct ext2_fifo *fifo = ext2_fifo_get(inode);
    struct proc *pr = curr_proc();

    acquire(&fifo->lock);
    int i = 0;
    while (i < len) {
        if (iskilled(pr)) {
            release(&fifo->lock);
            return -1;
        }
        if (fifo->nwrite == fifo->nread + EXT2_FIFO_SIZE) {
            wakeup(&fifo->nread);
            sleep(&fifo->nwrite, &fifo->lock);
            continue;
        }

        char ch = 0;
        vfs_either_copy_in((void *)((uint64)buf + i), &ch, 1);
        fifo->data[fifo->nwrite++ % EXT2_FIFO_SIZE] = ch;
        i++;
    }

    wakeup(&fifo->nread);
    release(&fifo->lock);
    return i;
}

static struct sb_operations ext2_sb_ops = {
    .free_inode  = ext2_free_inode_private,
    .write_inode = ext2_write_inode_private,
    .delete_inode = ext2_delete_inode,
};

static struct inode_operations ext2_iops = {
    .lookup = ext2_lookup,
    .create = ext2_create,
    .unlink = ext2_unlink,
    .mkdir  = ext2_mkdir,
    .rmdir  = ext2_rmdir,
    .mkfifo = ext2_mkfifo,
};

static struct file_operations ext2_reg_fops = {
    .read  = ext2_read,
    .write = ext2_write,
    .close = generic_file_close,
};

static struct file_operations ext2_dir_fops = {
    .iterate = ext2_iterate,
    .close = generic_file_close,
};

static struct file_operations ext2_fifo_fops = {
    .read  = ext2_fifo_read,
    .write = ext2_fifo_write,
    .close = generic_file_close,
};

int ext2_vfs_init(void) {
    if (ext2_init() < 0)
        return -EINVAL;

    allocator_init(&ext2_inode_allocator, "ext2_inode_info", sizeof(struct ext2_inode_info), 1024);
    allocator_init(&ext2_fifo_allocator, "ext2_fifo", sizeof(struct ext2_fifo), 128);

    ext2_sb.name = "ext2";
    ext2_sb.ops = &ext2_sb_ops;
    ext2_sb.private = ext2_get_fs();
    ext2_sb.list = NULL;
    spinlock_init(&ext2_sb.lock, "ext2 sb lock");

    struct inode *root = ext2_iget(&ext2_sb, 2);
    if (root == NULL)
        return -EINVAL;

    uint32 journal_ino = 0;
    if (ext2_find_root_entry(root, ".ext2_journal", &journal_ino) == 0) {
        struct inode *journal = ext2_iget(&ext2_sb, journal_ino);
        if (journal == NULL) {
            iunlockput(root);
            return -EINVAL;
        }
        iunlock(journal);
        int jret = ext2_journal_init(journal);
        if (jret < 0) {
            iput(journal);
            iunlockput(root);
            return jret;
        }
        // Keep one reference for the journal's lifetime; the mount owns it.
    }

    iunlock(root);

    ext2_sb.root = root;
    rootfs = &ext2_sb;
    return 0;
}

void fs_mount_root(void) {
    if (ext2_probe()) {
        if (ext2_vfs_init() == 0)
            return;
    }
    sfs_vfs_init();
}
