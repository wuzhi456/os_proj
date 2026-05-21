#include "ext2.h"

#include "buf.h"
#include "debug.h"
#include "defs.h"
#include "kalloc.h"
#include "log.h"

struct ext2_fs {
    struct ext2_superblock sb;
    uint32 block_size;
    uint32 blocks_per_group;
    uint32 inodes_per_group;
    uint32 inode_size;
    uint32 groups_count;
    uint32 first_data_block;
    uint32 group_desc_start;
    uint32 ptrs_per_block;
    uint32 max_file_blocks;
    struct ext2_group_desc *group_descs;
    spinlock_t alloc_lock;
};

struct ext2_inode_info {
    struct ext2_inode dinode;
    void *fifo_state;
};

static struct ext2_fs ext2sb;
static allocator_t ext2_inode_allocator;
static int ext2_inited = 0;

struct ext2_inode_info *ext2_inode_info(struct inode *inode) {
    return (struct ext2_inode_info *)inode->private;
}

uint32 ext2_block_size(void) {
    return ext2sb.block_size;
}

static void ext2_read_block(uint32 blockno, void *buf) {
    uint32 byte_off = blockno * ext2sb.block_size;
    uint32 dev_block = byte_off / BSIZE;
    uint32 dev_off = byte_off % BSIZE;
    assert(ext2sb.block_size <= BSIZE);
    assert(dev_off + ext2sb.block_size <= BSIZE);
    struct buf *bp = bread(0, dev_block);
    memmove(buf, bp->data + dev_off, ext2sb.block_size);
    brelse(bp);
}

static void ext2_write_block(uint32 blockno, const void *buf) {
    uint32 byte_off = blockno * ext2sb.block_size;
    uint32 dev_block = byte_off / BSIZE;
    uint32 dev_off = byte_off % BSIZE;
    assert(ext2sb.block_size <= BSIZE);
    assert(dev_off + ext2sb.block_size <= BSIZE);
    struct buf *bp = bread(0, dev_block);
    memmove(bp->data + dev_off, buf, ext2sb.block_size);
    bwrite(bp);
    brelse(bp);
}

static void ext2_zero_block(uint32 blockno) {
    uint8 buf[BSIZE];
    memset(buf, 0, ext2sb.block_size);
    ext2_write_block(blockno, buf);
}

static void ext2_read_superblock(struct ext2_superblock *out) {
    struct buf *bp = bread(0, 0);
    memmove(out, bp->data + 1024, sizeof(*out));
    brelse(bp);
}

static void ext2_write_superblock(void) {
    struct buf *bp = bread(0, 0);
    memmove(bp->data + 1024, &ext2sb.sb, sizeof(ext2sb.sb));
    bwrite(bp);
    brelse(bp);
}

static void ext2_write_group_desc(uint32 group) {
    uint32 offset = group * sizeof(struct ext2_group_desc);
    uint32 blk = ext2sb.group_desc_start + offset / ext2sb.block_size;
    uint32 blk_off = offset % ext2sb.block_size;
    uint8 buf[BSIZE];
    ext2_read_block(blk, buf);
    memmove(buf + blk_off, &ext2sb.group_descs[group], sizeof(struct ext2_group_desc));
    ext2_write_block(blk, buf);
}

static int ext2_read_inode_raw(uint32 ino, struct ext2_inode *out) {
    if (ino == 0 || ino > ext2sb.sb.s_inodes_count)
        return -EINVAL;
    uint32 group = (ino - 1) / ext2sb.inodes_per_group;
    uint32 index = (ino - 1) % ext2sb.inodes_per_group;
    uint32 inode_table = ext2sb.group_descs[group].bg_inode_table;
    uint32 offset = index * ext2sb.inode_size;
    uint32 blk = inode_table + offset / ext2sb.block_size;
    uint32 blk_off = offset % ext2sb.block_size;
    uint8 buf[BSIZE];
    ext2_read_block(blk, buf);
    memmove(out, buf + blk_off, sizeof(*out));
    return 0;
}

static int ext2_write_inode_raw(uint32 ino, const struct ext2_inode *in) {
    if (ino == 0 || ino > ext2sb.sb.s_inodes_count)
        return -EINVAL;
    uint32 group = (ino - 1) / ext2sb.inodes_per_group;
    uint32 index = (ino - 1) % ext2sb.inodes_per_group;
    uint32 inode_table = ext2sb.group_descs[group].bg_inode_table;
    uint32 offset = index * ext2sb.inode_size;
    uint32 blk = inode_table + offset / ext2sb.block_size;
    uint32 blk_off = offset % ext2sb.block_size;
    uint8 buf[BSIZE];
    ext2_read_block(blk, buf);
    memmove(buf + blk_off, in, sizeof(*in));
    ext2_write_block(blk, buf);
    return 0;
}

static void ext2_zero_inode(uint32 ino) {
    struct ext2_inode zero;
    memset(&zero, 0, sizeof(zero));
    ext2_write_inode_raw(ino, &zero);
}

static int ext2_bitmap_get(uint8 *bitmap, uint32 idx) {
    return (bitmap[idx / 8] >> (idx % 8)) & 1;
}

static void ext2_bitmap_set(uint8 *bitmap, uint32 idx, int val) {
    if (val)
        bitmap[idx / 8] |= (1u << (idx % 8));
    else
        bitmap[idx / 8] &= ~(1u << (idx % 8));
}

static int ext2_alloc_block(uint32 *out_blkno) {
    acquire(&ext2sb.alloc_lock);
    for (uint32 group = 0; group < ext2sb.groups_count; group++) {
        if (ext2sb.group_descs[group].bg_free_blocks_count == 0)
            continue;
        uint32 bitmap_blk = ext2sb.group_descs[group].bg_block_bitmap;
        uint8 buf[BSIZE];
        ext2_read_block(bitmap_blk, buf);
        uint32 group_first = ext2sb.first_data_block + group * ext2sb.blocks_per_group;
        for (uint32 bit = 0; bit < ext2sb.blocks_per_group; bit++) {
            uint32 blkno = group_first + bit;
            if (blkno >= ext2sb.sb.s_blocks_count)
                break;
            if (!ext2_bitmap_get(buf, bit)) {
                ext2_bitmap_set(buf, bit, 1);
                ext2_write_block(bitmap_blk, buf);
                ext2sb.group_descs[group].bg_free_blocks_count--;
                ext2sb.sb.s_free_blocks_count--;
                ext2_write_group_desc(group);
                ext2_write_superblock();
                release(&ext2sb.alloc_lock);
                ext2_zero_block(blkno);
                *out_blkno = blkno;
                return 0;
            }
        }
    }
    release(&ext2sb.alloc_lock);
    return -ENOSPC;
}

static void ext2_free_block(uint32 blkno) {
    if (blkno < ext2sb.first_data_block || blkno >= ext2sb.sb.s_blocks_count)
        return;
    uint32 group = (blkno - ext2sb.first_data_block) / ext2sb.blocks_per_group;
    uint32 bit = (blkno - ext2sb.first_data_block) % ext2sb.blocks_per_group;
    uint32 bitmap_blk = ext2sb.group_descs[group].bg_block_bitmap;
    uint8 buf[BSIZE];
    ext2_read_block(bitmap_blk, buf);
    if (ext2_bitmap_get(buf, bit)) {
        ext2_bitmap_set(buf, bit, 0);
        ext2_write_block(bitmap_blk, buf);
        ext2sb.group_descs[group].bg_free_blocks_count++;
        ext2sb.sb.s_free_blocks_count++;
        ext2_write_group_desc(group);
        ext2_write_superblock();
    }
}

static int ext2_alloc_inode(uint32 *out_ino) {
    acquire(&ext2sb.alloc_lock);
    for (uint32 group = 0; group < ext2sb.groups_count; group++) {
        if (ext2sb.group_descs[group].bg_free_inodes_count == 0)
            continue;
        uint32 bitmap_blk = ext2sb.group_descs[group].bg_inode_bitmap;
        uint8 buf[BSIZE];
        ext2_read_block(bitmap_blk, buf);
        for (uint32 bit = 0; bit < ext2sb.inodes_per_group; bit++) {
            uint32 ino = group * ext2sb.inodes_per_group + bit + 1;
            if (ino > ext2sb.sb.s_inodes_count)
                break;
            if (!ext2_bitmap_get(buf, bit)) {
                ext2_bitmap_set(buf, bit, 1);
                ext2_write_block(bitmap_blk, buf);
                ext2sb.group_descs[group].bg_free_inodes_count--;
                ext2sb.sb.s_free_inodes_count--;
                ext2_write_group_desc(group);
                ext2_write_superblock();
                release(&ext2sb.alloc_lock);
                ext2_zero_inode(ino);
                *out_ino = ino;
                return 0;
            }
        }
    }
    release(&ext2sb.alloc_lock);
    return -ENOSPC;
}

static void ext2_free_inode(uint32 ino) {
    if (ino == 0 || ino > ext2sb.sb.s_inodes_count)
        return;
    uint32 group = (ino - 1) / ext2sb.inodes_per_group;
    uint32 bit = (ino - 1) % ext2sb.inodes_per_group;
    uint32 bitmap_blk = ext2sb.group_descs[group].bg_inode_bitmap;
    uint8 buf[BSIZE];
    ext2_read_block(bitmap_blk, buf);
    if (ext2_bitmap_get(buf, bit)) {
        ext2_bitmap_set(buf, bit, 0);
        ext2_write_block(bitmap_blk, buf);
        ext2sb.group_descs[group].bg_free_inodes_count++;
        ext2sb.sb.s_free_inodes_count++;
        ext2_write_group_desc(group);
        ext2_write_superblock();
    }
    ext2_zero_inode(ino);
}

void ext2_free_inode_no(uint32 ino) {
    ext2_free_inode(ino);
}

static int ext2_get_block(struct ext2_inode_info *info, uint32 ino, uint32 file_block, int create, uint32 *out_blkno) {
    uint32 ptrs_per_block = ext2sb.ptrs_per_block;
    if (file_block >= ext2sb.max_file_blocks)
        return -EFBIG;

    if (file_block < EXT2_NDIR_BLOCKS) {
        uint32 blkno = info->dinode.i_block[file_block];
        if (blkno == 0 && create) {
            int ret = ext2_alloc_block(&blkno);
            if (ret < 0)
                return ret;
            info->dinode.i_block[file_block] = blkno;
            ext2_write_inode_raw(ino, &info->dinode);
        }
        *out_blkno = blkno;
        return 0;
    }

    uint8 buf[BSIZE];
    if (file_block < EXT2_NDIR_BLOCKS + ptrs_per_block) {
        uint32 idx = file_block - EXT2_NDIR_BLOCKS;
        uint32 ind_blk = info->dinode.i_block[EXT2_IND_BLOCK];
        if (ind_blk == 0) {
            if (!create) {
                *out_blkno = 0;
                return 0;
            }
            int ret = ext2_alloc_block(&ind_blk);
            if (ret < 0)
                return ret;
            info->dinode.i_block[EXT2_IND_BLOCK] = ind_blk;
            ext2_write_inode_raw(ino, &info->dinode);
        }
        ext2_read_block(ind_blk, buf);
        uint32 *ptrs = (uint32 *)buf;
        uint32 blkno = ptrs[idx];
        if (blkno == 0 && create) {
            int ret = ext2_alloc_block(&blkno);
            if (ret < 0)
                return ret;
            ptrs[idx] = blkno;
            ext2_write_block(ind_blk, buf);
        }
        *out_blkno = blkno;
        return 0;
    }

    uint32 remaining = file_block - EXT2_NDIR_BLOCKS - ptrs_per_block;
    uint32 per_dind = ptrs_per_block * ptrs_per_block;
    if (remaining < per_dind) {
        uint32 dind_blk = info->dinode.i_block[EXT2_DIND_BLOCK];
        if (dind_blk == 0) {
            if (!create) {
                *out_blkno = 0;
                return 0;
            }
            int ret = ext2_alloc_block(&dind_blk);
            if (ret < 0)
                return ret;
            info->dinode.i_block[EXT2_DIND_BLOCK] = dind_blk;
            ext2_write_inode_raw(ino, &info->dinode);
        }
        uint32 idx1 = remaining / ptrs_per_block;
        uint32 idx2 = remaining % ptrs_per_block;
        ext2_read_block(dind_blk, buf);
        uint32 *ptrs1 = (uint32 *)buf;
        uint32 ind_blk = ptrs1[idx1];
        if (ind_blk == 0) {
            if (!create) {
                *out_blkno = 0;
                return 0;
            }
            int ret = ext2_alloc_block(&ind_blk);
            if (ret < 0)
                return ret;
            ptrs1[idx1] = ind_blk;
            ext2_write_block(dind_blk, buf);
        }
        ext2_read_block(ind_blk, buf);
        uint32 *ptrs2 = (uint32 *)buf;
        uint32 blkno = ptrs2[idx2];
        if (blkno == 0 && create) {
            int ret = ext2_alloc_block(&blkno);
            if (ret < 0)
                return ret;
            ptrs2[idx2] = blkno;
            ext2_write_block(ind_blk, buf);
        }
        *out_blkno = blkno;
        return 0;
    }

    remaining -= per_dind;
    uint32 per_tind = ptrs_per_block * ptrs_per_block * ptrs_per_block;
    if (remaining >= per_tind)
        return -EFBIG;

    uint32 tind_blk = info->dinode.i_block[EXT2_TIND_BLOCK];
    if (tind_blk == 0) {
        if (!create) {
            *out_blkno = 0;
            return 0;
        }
        int ret = ext2_alloc_block(&tind_blk);
        if (ret < 0)
            return ret;
        info->dinode.i_block[EXT2_TIND_BLOCK] = tind_blk;
        ext2_write_inode_raw(ino, &info->dinode);
    }
    uint32 idx1 = remaining / per_dind;
    uint32 idx2 = (remaining / ptrs_per_block) % ptrs_per_block;
    uint32 idx3 = remaining % ptrs_per_block;
    ext2_read_block(tind_blk, buf);
    uint32 *ptrs1 = (uint32 *)buf;
    uint32 dind_blk = ptrs1[idx1];
    if (dind_blk == 0) {
        if (!create) {
            *out_blkno = 0;
            return 0;
        }
        int ret = ext2_alloc_block(&dind_blk);
        if (ret < 0)
            return ret;
        ptrs1[idx1] = dind_blk;
        ext2_write_block(tind_blk, buf);
    }
    ext2_read_block(dind_blk, buf);
    uint32 *ptrs2 = (uint32 *)buf;
    uint32 ind_blk = ptrs2[idx2];
    if (ind_blk == 0) {
        if (!create) {
            *out_blkno = 0;
            return 0;
        }
        int ret = ext2_alloc_block(&ind_blk);
        if (ret < 0)
            return ret;
        ptrs2[idx2] = ind_blk;
        ext2_write_block(dind_blk, buf);
    }
    ext2_read_block(ind_blk, buf);
    uint32 *ptrs3 = (uint32 *)buf;
    uint32 blkno = ptrs3[idx3];
    if (blkno == 0 && create) {
        int ret = ext2_alloc_block(&blkno);
        if (ret < 0)
            return ret;
        ptrs3[idx3] = blkno;
        ext2_write_block(ind_blk, buf);
    }
    *out_blkno = blkno;
    return 0;
}

static void ext2_free_indirect(uint32 blkno, int depth) {
    if (blkno == 0)
        return;
    if (depth == 0) {
        ext2_free_block(blkno);
        return;
    }
    uint8 buf[BSIZE];
    ext2_read_block(blkno, buf);
    uint32 *ptrs = (uint32 *)buf;
    uint32 count = ext2sb.block_size / sizeof(uint32);
    for (uint32 i = 0; i < count; i++) {
        if (ptrs[i] != 0)
            ext2_free_indirect(ptrs[i], depth - 1);
    }
    ext2_free_block(blkno);
}

static void ext2_clear_inode_blocks(struct ext2_inode_info *info) {
    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (info->dinode.i_block[i] != 0) {
            ext2_free_block(info->dinode.i_block[i]);
            info->dinode.i_block[i] = 0;
        }
    }
    if (info->dinode.i_block[EXT2_IND_BLOCK]) {
        ext2_free_indirect(info->dinode.i_block[EXT2_IND_BLOCK], 1);
        info->dinode.i_block[EXT2_IND_BLOCK] = 0;
    }
    if (info->dinode.i_block[EXT2_DIND_BLOCK]) {
        ext2_free_indirect(info->dinode.i_block[EXT2_DIND_BLOCK], 2);
        info->dinode.i_block[EXT2_DIND_BLOCK] = 0;
    }
    if (info->dinode.i_block[EXT2_TIND_BLOCK]) {
        ext2_free_indirect(info->dinode.i_block[EXT2_TIND_BLOCK], 3);
        info->dinode.i_block[EXT2_TIND_BLOCK] = 0;
    }
}

int ext2_probe(void) {
    struct ext2_superblock sb;
    ext2_read_superblock(&sb);
    return sb.s_magic == EXT2_SUPER_MAGIC;
}

void ext2_init(void) {
    if (ext2_inited)
        return;
    ext2_inited = 1;

    memset(&ext2sb, 0, sizeof(ext2sb));
    ext2_read_superblock(&ext2sb.sb);
    if (ext2sb.sb.s_magic != EXT2_SUPER_MAGIC) {
        panic("ext2: bad superblock");
    }

    ext2sb.block_size = 1024u << ext2sb.sb.s_log_block_size;
    assert(ext2sb.block_size != 0);
    assert(ext2sb.block_size <= BSIZE);
    ext2sb.ptrs_per_block = ext2sb.block_size / sizeof(uint32);
    uint64 max_blocks = EXT2_NDIR_BLOCKS + ext2sb.ptrs_per_block;
    max_blocks += (uint64)ext2sb.ptrs_per_block * ext2sb.ptrs_per_block;
    max_blocks += (uint64)ext2sb.ptrs_per_block * ext2sb.ptrs_per_block * ext2sb.ptrs_per_block;
    ext2sb.max_file_blocks = (uint32)max_blocks;
    ext2sb.blocks_per_group = ext2sb.sb.s_blocks_per_group;
    ext2sb.inodes_per_group = ext2sb.sb.s_inodes_per_group;
    ext2sb.inode_size = ext2sb.sb.s_inode_size ? ext2sb.sb.s_inode_size : sizeof(struct ext2_inode);
    ext2sb.first_data_block = ext2sb.sb.s_first_data_block;
    ext2sb.groups_count =
        (ext2sb.sb.s_blocks_count - ext2sb.first_data_block + ext2sb.blocks_per_group - 1) / ext2sb.blocks_per_group;
    ext2sb.group_desc_start = (ext2sb.block_size == 1024) ? 2 : 1;

    uint32 desc_bytes = ext2sb.groups_count * sizeof(struct ext2_group_desc);
    assert(desc_bytes <= PGSIZE);
    ext2sb.group_descs = (struct ext2_group_desc *)PA_TO_KVA(kallocpage());
    memset(ext2sb.group_descs, 0, PGSIZE);

    uint8 buf[BSIZE];
    uint32 remaining = desc_bytes;
    uint32 offset = 0;
    uint32 blk = ext2sb.group_desc_start;
    while (remaining > 0) {
        ext2_read_block(blk, buf);
        uint32 to_copy = MIN(remaining, ext2sb.block_size);
        memmove((uint8 *)ext2sb.group_descs + offset, buf, to_copy);
        remaining -= to_copy;
        offset += to_copy;
        blk++;
    }

    spinlock_init(&ext2sb.alloc_lock, "ext2_alloc");
    allocator_init(&ext2_inode_allocator, "ext2_inode", sizeof(struct ext2_inode_info), 1024);
}

struct inode *ext2_iget(struct superblock *sb, uint32 ino) {
    struct inode *ind = iget_locked(sb, ino);
    struct ext2_inode disk_inode;
    if (ext2_read_inode_raw(ino, &disk_inode) != 0) {
        iunlockput(ind);
        return NULL;
    }

    struct ext2_inode_info *info = kalloc(&ext2_inode_allocator);
    memset(info, 0, sizeof(*info));
    info->dinode = disk_inode;
    ind->private = info;
    ind->size = disk_inode.i_size;
    ind->nlinks = disk_inode.i_links_count;

    if (disk_inode.i_mode & EXT2_S_IFDIR) {
        ind->imode = IMODE_DIR;
    } else if (disk_inode.i_mode & EXT2_S_IFREG) {
        ind->imode = IMODE_REG;
    } else if (disk_inode.i_mode & EXT2_S_IFIFO) {
        ind->imode = IMODE_FIFO;
    } else {
        ind->imode = 0;
    }

    return ind;
}

int ext2_read_data(struct inode *inode, uint32 addr, void *__either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    struct ext2_inode_info *info = ext2_inode_info(inode);
    if (addr >= inode->size)
        return 0;

    loff_t pos = addr;
    loff_t end = MIN((uint64)addr + len, inode->size);
    uint8 block_buf[BSIZE];

    while (pos < end) {
        uint32 file_block = pos / ext2sb.block_size;
        uint32 blk_off = pos % ext2sb.block_size;
        uint32 to_read = MIN((uint64)end - pos, ext2sb.block_size - blk_off);
        uint32 blkno = 0;
        int ret = ext2_get_block(info, inode->ino, file_block, 0, &blkno);
        if (ret < 0)
            return ret;
        if (blkno == 0) {
            memset(block_buf, 0, to_read);
            vfs_either_copy_out(buf, block_buf, to_read);
        } else {
            ext2_read_block(blkno, block_buf);
            vfs_either_copy_out(buf, block_buf + blk_off, to_read);
        }
        pos += to_read;
        buf = (void *)((uint64)buf + to_read);
    }
    return pos - addr;
}

int ext2_write_data(struct inode *inode, uint32 addr, void *__either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    struct ext2_inode_info *info = ext2_inode_info(inode);
    loff_t pos = addr;
    loff_t end = addr + len;
    uint8 block_buf[BSIZE];
    int ret = 0;

    while (pos < end) {
        uint32 file_block = pos / ext2sb.block_size;
        uint32 blk_off = pos % ext2sb.block_size;
        uint32 to_write = MIN((uint64)end - pos, ext2sb.block_size - blk_off);
        uint32 blkno = 0;
        ret = ext2_get_block(info, inode->ino, file_block, 1, &blkno);
        if (ret < 0)
            return ret;
        if (blkno == 0)
            return -ENOSPC;
        ext2_read_block(blkno, block_buf);
        vfs_either_copy_in(buf, block_buf + blk_off, to_write);
        ext2_write_block(blkno, block_buf);
        pos += to_write;
        buf = (void *)((uint64)buf + to_write);
    }

    if (pos > inode->size) {
        inode->size = pos;
        info->dinode.i_size = pos;
        ext2_write_inode_raw(inode->ino, &info->dinode);
    }
    return pos - addr;
}

void ext2_truncate_inode(struct inode *inode) {
    assert(holdingsleep(&inode->lock));
    struct ext2_inode_info *info = ext2_inode_info(inode);
    ext2_clear_inode_blocks(info);
    inode->size = 0;
    info->dinode.i_size = 0;
    ext2_write_inode_raw(inode->ino, &info->dinode);
}

void ext2_delete_inode(struct inode *inode) {
    struct ext2_inode_info *info = ext2_inode_info(inode);
    ext2_clear_inode_blocks(info);
    info->dinode.i_size = 0;
    info->dinode.i_links_count = 0;
    ext2_write_inode_raw(inode->ino, &info->dinode);
    ext2_free_inode(inode->ino);
}

void ext2_write_inode(struct inode *inode) {
    struct ext2_inode_info *info = ext2_inode_info(inode);
    info->dinode.i_size = inode->size;
    info->dinode.i_links_count = inode->nlinks;
    ext2_write_inode_raw(inode->ino, &info->dinode);
}

void ext2_free_inode_info(struct inode *inode) {
    struct ext2_inode_info *info = ext2_inode_info(inode);
    if (info != NULL) {
        if (info->fifo_state != NULL) {
            kfreepage((void *)KVA_TO_PA(info->fifo_state));
            info->fifo_state = NULL;
        }
        kfree(&ext2_inode_allocator, info);
    }
}

int ext2_alloc_inode_with_mode(uint16 mode, uint32 *out_ino, struct ext2_inode *out_inode) {
    uint32 ino = 0;
    int ret = ext2_alloc_inode(&ino);
    if (ret < 0)
        return ret;

    struct ext2_inode dinode;
    memset(&dinode, 0, sizeof(dinode));
    dinode.i_mode = mode;
    dinode.i_links_count = 1;
    dinode.i_size = 0;
    ext2_write_inode_raw(ino, &dinode);

    if (out_inode)
        *out_inode = dinode;
    *out_ino = ino;
    return 0;
}
