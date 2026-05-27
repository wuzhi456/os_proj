#include "ext2/ext2.h"

#include "defs.h"
#include "fs/buf.h"

#define EXT2_MAX_GROUPS 128
#define EXT2_JOURNAL_MAGIC 0x324a4e4c
#define EXT2_JOURNAL_FILE_BLOCKS 2

struct ext2_block_view {
    struct buf *buf;
    uint32 eblk;
    uint8 *data;
};

static struct ext2_fs ext2_state;
static struct ext2_group_desc ext2_groups[EXT2_MAX_GROUPS];
static int ext2_inited = 0;
static uint8 ext2_zero_buf[BSIZE];
static struct {
    struct inode *inode;
    sleeplock_t lock;
    uint32 seq;
    int ready;
    int bypass;
} ext2_journal;

static int ext2_journal_replay(void);
static int ext2_journal_clear(void);

static int ext2_block_to_disk(uint32 eblk, uint32 *disk_blk, uint32 *offset) {
    uint32 bs = ext2_state.block_size;
    if (bs == 0 || bs > BSIZE)
        return -EINVAL;
    if (BSIZE % bs != 0)
        return -EINVAL;

    uint64 byte_off = (uint64)eblk * bs;
    *disk_blk = (uint32)(byte_off / BSIZE);
    *offset = (uint32)(byte_off % BSIZE);
    return 0;
}

static int ext2_bread(uint32 eblk, struct ext2_block_view *view) {
    uint32 disk_blk = 0;
    uint32 offset = 0;
    int ret = ext2_block_to_disk(eblk, &disk_blk, &offset);
    if (ret < 0)
        return ret;

    struct buf *b = bread(0, disk_blk);
    view->buf = b;
    view->eblk = eblk;
    view->data = b->data + offset;
    return 0;
}

static void ext2_brelse(struct ext2_block_view *view);

static void ext2_journal_bypass_begin(void) {
    __sync_add_and_fetch(&ext2_journal.bypass, 1);
}

static void ext2_journal_bypass_end(void) {
    int v = __sync_sub_and_fetch(&ext2_journal.bypass, 1);
    assert(v >= 0);
}

static int ext2_journal_write_raw(loff_t off, void *buf, loff_t len) {
    if (ext2_journal.inode == NULL)
        return 0;

    ilock(ext2_journal.inode);
    ext2_journal_bypass_begin();
    int ret = ext2_writei(ext2_journal.inode, off, buf, len);
    ext2_journal_bypass_end();
    iunlock(ext2_journal.inode);
    return ret == len ? 0 : -EINVAL;
}

static int ext2_journal_read_raw(loff_t off, void *buf, loff_t len) {
    if (ext2_journal.inode == NULL)
        return -EINVAL;

    ilock(ext2_journal.inode);
    int ret = ext2_readi(ext2_journal.inode, off, buf, len);
    iunlock(ext2_journal.inode);
    return ret == len ? 0 : -EINVAL;
}

static int ext2_journal_store_header(struct ext2_journal_hdr *hdr) {
    uint32 bs = ext2_state.block_size;
    uint8 *tmp = ext2_zero_buf;
    if (bs > sizeof(ext2_zero_buf))
        return -EINVAL;

    memset(tmp, 0, bs);
    memmove(tmp, hdr, sizeof(*hdr));
    return ext2_journal_write_raw(0, tmp, bs);
}

static int ext2_journal_store_payload(uint8 *payload) {
    return ext2_journal_write_raw(ext2_state.block_size, payload, ext2_state.block_size);
}

static int ext2_journal_clear(void) {
    struct ext2_journal_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = EXT2_JOURNAL_MAGIC;
    return ext2_journal_store_header(&hdr);
}

static int ext2_journal_apply(uint32 target_block, uint8 *payload) {
    struct ext2_block_view view;
    int ret = ext2_bread(target_block, &view);
    if (ret < 0)
        return ret;
    memmove(view.data, payload, ext2_state.block_size);
    bwrite(view.buf);
    ext2_brelse(&view);
    return 0;
}

static int ext2_journal_replay(void) {
    if (ext2_journal.inode == NULL)
        return 0;

    uint32 bs = ext2_state.block_size;
    if (bs > BSIZE)
        return -EINVAL;

    void *pa = kallocpage();
    if (pa == NULL)
        return -ENOMEM;
    uint8 *buf = (uint8 *)PA_TO_KVA(pa);

    struct ext2_journal_hdr hdr;
    memset(&hdr, 0, sizeof(hdr));

    ext2_journal_bypass_begin();
    int ret = ext2_journal_read_raw(0, buf, bs);
    if (ret < 0) {
        ext2_journal_bypass_end();
        kfreepage(pa);
        return ret;
    }
    memmove(&hdr, buf, sizeof(hdr));
    // Recovery cases:
    // - COMMITTED: redo the recorded block image.
    // - PREPARED: discard the partial record and clear the journal.
    // - invalid magic: reinitialize the journal header.
    if (hdr.magic == EXT2_JOURNAL_MAGIC && hdr.state == EXT2_JOURNAL_COMMITTED) {
        ret = ext2_journal_read_raw(bs, buf, bs);
        if (ret >= 0)
            ret = ext2_journal_apply(hdr.target_block, buf);
        if (ret >= 0)
            ret = ext2_journal_clear();
    } else if (hdr.magic == EXT2_JOURNAL_MAGIC && hdr.state == EXT2_JOURNAL_PREPARED) {
        ret = ext2_journal_clear();
    } else if (hdr.magic != EXT2_JOURNAL_MAGIC) {
        ret = ext2_journal_clear();
    }
    ext2_journal_bypass_end();
    kfreepage(pa);
    return ret;
}

static void ext2_bwrite(struct ext2_block_view *view) {
    if (ext2_journal.ready && ext2_journal.inode != NULL && __sync_fetch_and_add(&ext2_journal.bypass, 0) == 0) {
        struct ext2_journal_hdr hdr;
        uint32 bs = ext2_state.block_size;
        if (bs <= BSIZE) {
            acquiresleep(&ext2_journal.lock);
            hdr.magic = EXT2_JOURNAL_MAGIC;
            hdr.state = EXT2_JOURNAL_PREPARED;
            hdr.target_block = view->eblk;
            hdr.seq = ext2_journal.seq++;
            if (ext2_journal_store_header(&hdr) >= 0 && ext2_journal_store_payload(view->data) >= 0) {
                hdr.state = EXT2_JOURNAL_COMMITTED;
                if (ext2_journal_store_header(&hdr) < 0) {
                    warnf("ext2 journal commit failed for block %u", view->eblk);
                    if (ext2_journal_clear() < 0)
                        warnf("ext2 journal clear failed for block %u", view->eblk);
                } else {
                    // debug:journaling写路径写成功了也提醒
                    warnf("ext2 journal committed: seq=%u target_block=%u", hdr.seq, hdr.target_block);
                }
            } else {
                warnf("ext2 journal prepare failed for block %u", view->eblk);
                if (ext2_journal_clear() < 0)
                    warnf("ext2 journal clear failed for block %u", view->eblk);
            }
            releasesleep(&ext2_journal.lock);
        }
    }
    bwrite(view->buf);
}

static void ext2_brelse(struct ext2_block_view *view) {
    brelse(view->buf);
}

static int ext2_zero_block(uint32 eblk) {
    struct ext2_block_view view;
    int ret = ext2_bread(eblk, &view);
    if (ret < 0)
        return ret;
    memset(view.data, 0, ext2_state.block_size);
    ext2_bwrite(&view);
    ext2_brelse(&view);
    return 0;
}

static int ext2_read_superblock(struct ext2_superblock *out) {
    struct buf *b = bread(0, 0);
    memmove(out, b->data + 1024, sizeof(*out));
    brelse(b);
    if (out->s_magic != EXT2_SUPER_MAGIC)
        return -EINVAL;
    return 0;
}

static int ext2_write_superblock(void) {
    struct buf *b = bread(0, 0);
    memmove(b->data + 1024, &ext2_state.sb, sizeof(ext2_state.sb));
    bwrite(b);
    brelse(b);
    return 0;
}

static int ext2_read_group_desc(uint32 group, struct ext2_group_desc *out) {
    uint32 desc_per_block = ext2_state.block_size / sizeof(struct ext2_group_desc);
    uint32 gdt_start = ext2_state.sb.s_first_data_block + 1;
    uint32 block = gdt_start + (group / desc_per_block);
    uint32 offset = (group % desc_per_block) * sizeof(struct ext2_group_desc);

    struct ext2_block_view view;
    int ret = ext2_bread(block, &view);
    if (ret < 0)
        return ret;
    memmove(out, view.data + offset, sizeof(*out));
    ext2_brelse(&view);
    return 0;
}

static int ext2_write_group_desc(uint32 group, struct ext2_group_desc *in) {
    uint32 desc_per_block = ext2_state.block_size / sizeof(struct ext2_group_desc);
    uint32 gdt_start = ext2_state.sb.s_first_data_block + 1;
    uint32 block = gdt_start + (group / desc_per_block);
    uint32 offset = (group % desc_per_block) * sizeof(struct ext2_group_desc);

    struct ext2_block_view view;
    int ret = ext2_bread(block, &view);
    if (ret < 0)
        return ret;
    memmove(view.data + offset, in, sizeof(*in));
    ext2_bwrite(&view);
    ext2_brelse(&view);
    return 0;
}

static uint32 ext2_blocks_per_group(void) {
    return ext2_state.sb.s_blocks_per_group;
}

static uint32 ext2_inodes_per_group(void) {
    return ext2_state.sb.s_inodes_per_group;
}

static uint32 ext2_group_start_block(uint32 group) {
    return ext2_state.sb.s_first_data_block + group * ext2_blocks_per_group();
}

static void ext2_bitmap_set(uint8 *bitmap, uint32 idx, int val) {
    uint32 byte = idx / 8;
    uint32 bit = idx % 8;
    if (val)
        bitmap[byte] |= (1u << bit);
    else
        bitmap[byte] &= ~(1u << bit);
}

static int ext2_bitmap_get(uint8 *bitmap, uint32 idx) {
    uint32 byte = idx / 8;
    uint32 bit = idx % 8;
    return (bitmap[byte] >> bit) & 1u;
}

static int ext2_bitmap_find_zero(uint8 *bitmap, uint32 max_bits, uint32 *idx_out) {
    for (uint32 i = 0; i < max_bits; i++) {
        if (!ext2_bitmap_get(bitmap, i)) {
            *idx_out = i;
            return 0;
        }
    }
    return -ENOENT;
}

int ext2_probe(void) {
    struct ext2_superblock sb;
    if (ext2_read_superblock(&sb) < 0)
        return 0;
    return sb.s_magic == EXT2_SUPER_MAGIC;
}

struct ext2_fs *ext2_get_fs(void) {
    return &ext2_state;
}

int ext2_init(void) {
    if (ext2_inited)
        return 0;

    struct ext2_superblock sb;
    if (ext2_read_superblock(&sb) < 0)
        return -EINVAL;

    ext2_state.sb = sb;
    if (ext2_state.sb.s_inode_size == 0)
        ext2_state.sb.s_inode_size = 128;

    if (ext2_state.sb.s_inode_size < sizeof(struct ext2_inode))
        return -EINVAL;

    ext2_state.block_size = 1024u << ext2_state.sb.s_log_block_size;
    if (ext2_state.block_size > BSIZE)
        return -EINVAL;
    if (BSIZE % ext2_state.block_size != 0)
        return -EINVAL;

    uint32 blocks = ext2_state.sb.s_blocks_count - ext2_state.sb.s_first_data_block;
    uint32 groups = (blocks + ext2_blocks_per_group() - 1) / ext2_blocks_per_group();
    if (groups == 0 || groups > EXT2_MAX_GROUPS)
        return -EINVAL;

    ext2_state.group_count = groups;
    ext2_state.groups = ext2_groups;
    sleeplock_init(&ext2_state.lock, "ext2 lock");

    for (uint32 g = 0; g < ext2_state.group_count; g++) {
        if (ext2_read_group_desc(g, &ext2_state.groups[g]) < 0)
            return -EINVAL;
    }

    ext2_inited = 1;
    return 0;
}

int ext2_journal_init(struct inode *journal_inode) {
    if (journal_inode == NULL)
        return -EINVAL;

    uint32 bs = ext2_state.block_size;
    if (bs == 0 || bs > BSIZE)
        return -EINVAL;
    if (journal_inode->size < (loff_t)bs * EXT2_JOURNAL_FILE_BLOCKS)
        return -EINVAL;

    sleeplock_init(&ext2_journal.lock, "ext2 journal");
    ext2_journal.inode = journal_inode;
    ext2_journal.seq = 1;
    ext2_journal.ready = 0;
    ext2_journal.bypass = 0;

    int ret = ext2_journal_replay();
    if (ret < 0)
        return ret;

    ret = ext2_journal_clear();
    if (ret < 0)
        return ret;

    ext2_journal.ready = 1;
    return 0;
}

int ext2_read_inode(uint32 ino, struct ext2_inode *out) {
    if (ino == 0 || ino > ext2_state.sb.s_inodes_count)
        return -EINVAL;

    uint32 group = (ino - 1) / ext2_inodes_per_group();
    uint32 index = (ino - 1) % ext2_inodes_per_group();
    if (group >= ext2_state.group_count)
        return -EINVAL;

    struct ext2_group_desc *gd = &ext2_state.groups[group];
    uint32 inodes_per_block = ext2_state.block_size / ext2_state.sb.s_inode_size;
    uint32 block = gd->bg_inode_table + (index / inodes_per_block);
    uint32 offset = (index % inodes_per_block) * ext2_state.sb.s_inode_size;

    struct ext2_block_view view;
    int ret = ext2_bread(block, &view);
    if (ret < 0)
        return ret;

    memset(out, 0, sizeof(*out));
    uint32 copy_len = MIN((uint32)sizeof(*out), ext2_state.sb.s_inode_size);
    memmove(out, view.data + offset, copy_len);
    ext2_brelse(&view);
    return 0;
}

int ext2_write_inode(uint32 ino, struct ext2_inode *in) {
    if (ino == 0 || ino > ext2_state.sb.s_inodes_count)
        return -EINVAL;

    uint32 group = (ino - 1) / ext2_inodes_per_group();
    uint32 index = (ino - 1) % ext2_inodes_per_group();
    if (group >= ext2_state.group_count)
        return -EINVAL;

    struct ext2_group_desc *gd = &ext2_state.groups[group];
    uint32 inodes_per_block = ext2_state.block_size / ext2_state.sb.s_inode_size;
    uint32 block = gd->bg_inode_table + (index / inodes_per_block);
    uint32 offset = (index % inodes_per_block) * ext2_state.sb.s_inode_size;

    struct ext2_block_view view;
    int ret = ext2_bread(block, &view);
    if (ret < 0)
        return ret;

    uint32 copy_len = MIN((uint32)sizeof(*in), ext2_state.sb.s_inode_size);
    memmove(view.data + offset, in, copy_len);
    ext2_bwrite(&view);
    ext2_brelse(&view);
    return 0;
}

static void ext2_clear_inode(uint32 ino) {
    struct ext2_inode z;
    memset(&z, 0, sizeof(z));
    ext2_write_inode(ino, &z);
}

uint32 ext2_alloc_block(void) {
    acquiresleep(&ext2_state.lock);

    for (uint32 g = 0; g < ext2_state.group_count; g++) {
        struct ext2_group_desc *gd = &ext2_state.groups[g];
        if (gd->bg_free_blocks_count == 0)
            continue;

        struct ext2_block_view view;
        if (ext2_bread(gd->bg_block_bitmap, &view) < 0)
            continue;

        uint32 max_bits = ext2_blocks_per_group();
        for (uint32 bit = 0; bit < max_bits; bit++) {
            if (ext2_bitmap_get(view.data, bit))
                continue;

            uint32 blkno = ext2_group_start_block(g) + bit;
            if (blkno >= ext2_state.sb.s_blocks_count) {
                ext2_bitmap_set(view.data, bit, 1);
                continue;
            }

            ext2_bitmap_set(view.data, bit, 1);
            ext2_bwrite(&view);
            ext2_brelse(&view);

            gd->bg_free_blocks_count--;
            ext2_state.sb.s_free_blocks_count--;
            ext2_write_group_desc(g, gd);
            ext2_write_superblock();

            releasesleep(&ext2_state.lock);
            ext2_zero_block(blkno);
            return blkno;
        }

        ext2_bwrite(&view);
        ext2_brelse(&view);
    }

    releasesleep(&ext2_state.lock);
    return 0;
}

void ext2_free_block(uint32 blkno) {
    if (blkno < ext2_state.sb.s_first_data_block || blkno >= ext2_state.sb.s_blocks_count)
        return;

    acquiresleep(&ext2_state.lock);

    uint32 group = (blkno - ext2_state.sb.s_first_data_block) / ext2_blocks_per_group();
    uint32 bit = (blkno - ext2_state.sb.s_first_data_block) % ext2_blocks_per_group();
    if (group >= ext2_state.group_count) {
        releasesleep(&ext2_state.lock);
        return;
    }

    struct ext2_group_desc *gd = &ext2_state.groups[group];
    struct ext2_block_view view;
    if (ext2_bread(gd->bg_block_bitmap, &view) < 0) {
        releasesleep(&ext2_state.lock);
        return;
    }

    ext2_bitmap_set(view.data, bit, 0);
    ext2_bwrite(&view);
    ext2_brelse(&view);

    gd->bg_free_blocks_count++;
    ext2_state.sb.s_free_blocks_count++;
    ext2_write_group_desc(group, gd);
    ext2_write_superblock();

    releasesleep(&ext2_state.lock);
}

uint32 ext2_alloc_inode(void) {
    acquiresleep(&ext2_state.lock);

    for (uint32 g = 0; g < ext2_state.group_count; g++) {
        struct ext2_group_desc *gd = &ext2_state.groups[g];
        if (gd->bg_free_inodes_count == 0)
            continue;

        struct ext2_block_view view;
        if (ext2_bread(gd->bg_inode_bitmap, &view) < 0)
            continue;

        uint32 max_bits = ext2_inodes_per_group();
        int dirty = 0;
        for (uint32 bit = 0; bit < max_bits; bit++) {
            if (ext2_bitmap_get(view.data, bit))
                continue;

            uint32 ino = g * ext2_inodes_per_group() + bit + 1;
            if (ino < ext2_state.sb.s_first_ino) {
                ext2_bitmap_set(view.data, bit, 1);
                dirty = 1;
                continue;
            }

            ext2_bitmap_set(view.data, bit, 1);
            ext2_bwrite(&view);
            ext2_brelse(&view);

            gd->bg_free_inodes_count--;
            ext2_state.sb.s_free_inodes_count--;
            ext2_write_group_desc(g, gd);
            ext2_write_superblock();

            releasesleep(&ext2_state.lock);
            ext2_clear_inode(ino);
            return ino;
        }
        if (dirty)
            ext2_bwrite(&view);
        ext2_brelse(&view);
    }

    releasesleep(&ext2_state.lock);
    return 0;
}

void ext2_free_inode(uint32 ino) {
    if (ino == 0 || ino > ext2_state.sb.s_inodes_count)
        return;

    acquiresleep(&ext2_state.lock);

    uint32 group = (ino - 1) / ext2_inodes_per_group();
    uint32 bit = (ino - 1) % ext2_inodes_per_group();
    if (group >= ext2_state.group_count) {
        releasesleep(&ext2_state.lock);
        return;
    }

    struct ext2_group_desc *gd = &ext2_state.groups[group];
    struct ext2_block_view view;
    if (ext2_bread(gd->bg_inode_bitmap, &view) < 0) {
        releasesleep(&ext2_state.lock);
        return;
    }

    ext2_bitmap_set(view.data, bit, 0);
    ext2_bwrite(&view);
    ext2_brelse(&view);

    gd->bg_free_inodes_count++;
    ext2_state.sb.s_free_inodes_count++;
    ext2_write_group_desc(group, gd);
    ext2_write_superblock();

    releasesleep(&ext2_state.lock);
}

static void ext2_inode_blocks_add(struct ext2_inode_info *info, uint32 blocks) {
    uint32 sectors = (ext2_state.block_size / 512) * blocks;
    info->inode.i_blocks += sectors;
}

static int ext2_get_block(struct ext2_inode_info *info, uint32 lbn, int allocate, uint32 *out) {
    uint32 ptrs = ext2_state.block_size / sizeof(uint32);

    if (lbn < EXT2_NDIR_BLOCKS) {
        uint32 blkno = info->inode.i_block[lbn];
        if (blkno == 0 && allocate) {
            blkno = ext2_alloc_block();
            if (blkno == 0)
                return -ENOSPC;
            info->inode.i_block[lbn] = blkno;
            ext2_inode_blocks_add(info, 1);
        }
        *out = blkno;
        return 0;
    }

    lbn -= EXT2_NDIR_BLOCKS;
    if (lbn < ptrs) {
        uint32 ind = info->inode.i_block[EXT2_IND_BLOCK];
        if (ind == 0 && allocate) {
            ind = ext2_alloc_block();
            if (ind == 0)
                return -ENOSPC;
            ext2_zero_block(ind);
            info->inode.i_block[EXT2_IND_BLOCK] = ind;
            ext2_inode_blocks_add(info, 1);
        }
        if (ind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view;
        int ret = ext2_bread(ind, &view);
        if (ret < 0)
            return ret;
        uint32 *ptr = (uint32 *)view.data;
        uint32 blkno = ptr[lbn];
        if (blkno == 0 && allocate) {
            blkno = ext2_alloc_block();
            if (blkno == 0) {
                ext2_brelse(&view);
                return -ENOSPC;
            }
            ptr[lbn] = blkno;
            ext2_bwrite(&view);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view);
        *out = blkno;
        return 0;
    }

    lbn -= ptrs;
    if (lbn < ptrs * ptrs) {
        uint32 dind = info->inode.i_block[EXT2_DIND_BLOCK];
        if (dind == 0 && allocate) {
            dind = ext2_alloc_block();
            if (dind == 0)
                return -ENOSPC;
            ext2_zero_block(dind);
            info->inode.i_block[EXT2_DIND_BLOCK] = dind;
            ext2_inode_blocks_add(info, 1);
        }
        if (dind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view1;
        int ret = ext2_bread(dind, &view1);
        if (ret < 0)
            return ret;
        uint32 *ptr1 = (uint32 *)view1.data;
        uint32 idx1 = lbn / ptrs;
        uint32 idx2 = lbn % ptrs;

        uint32 ind = ptr1[idx1];
        if (ind == 0 && allocate) {
            ind = ext2_alloc_block();
            if (ind == 0) {
                ext2_brelse(&view1);
                return -ENOSPC;
            }
            ext2_zero_block(ind);
            ptr1[idx1] = ind;
            ext2_bwrite(&view1);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view1);
        if (ind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view2;
        ret = ext2_bread(ind, &view2);
        if (ret < 0)
            return ret;
        uint32 *ptr2 = (uint32 *)view2.data;
        uint32 blkno = ptr2[idx2];
        if (blkno == 0 && allocate) {
            blkno = ext2_alloc_block();
            if (blkno == 0) {
                ext2_brelse(&view2);
                return -ENOSPC;
            }
            ptr2[idx2] = blkno;
            ext2_bwrite(&view2);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view2);
        *out = blkno;
        return 0;
    }

    lbn -= ptrs * ptrs;
    if (lbn < ptrs * ptrs * ptrs) {
        uint32 tind = info->inode.i_block[EXT2_TIND_BLOCK];
        if (tind == 0 && allocate) {
            tind = ext2_alloc_block();
            if (tind == 0)
                return -ENOSPC;
            ext2_zero_block(tind);
            info->inode.i_block[EXT2_TIND_BLOCK] = tind;
            ext2_inode_blocks_add(info, 1);
        }
        if (tind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view1;
        int ret = ext2_bread(tind, &view1);
        if (ret < 0)
            return ret;
        uint32 *ptr1 = (uint32 *)view1.data;

        uint32 idx1 = lbn / (ptrs * ptrs);
        uint32 idx2 = (lbn / ptrs) % ptrs;
        uint32 idx3 = lbn % ptrs;

        uint32 dind = ptr1[idx1];
        if (dind == 0 && allocate) {
            dind = ext2_alloc_block();
            if (dind == 0) {
                ext2_brelse(&view1);
                return -ENOSPC;
            }
            ext2_zero_block(dind);
            ptr1[idx1] = dind;
            ext2_bwrite(&view1);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view1);
        if (dind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view2;
        ret = ext2_bread(dind, &view2);
        if (ret < 0)
            return ret;
        uint32 *ptr2 = (uint32 *)view2.data;

        uint32 ind = ptr2[idx2];
        if (ind == 0 && allocate) {
            ind = ext2_alloc_block();
            if (ind == 0) {
                ext2_brelse(&view2);
                return -ENOSPC;
            }
            ext2_zero_block(ind);
            ptr2[idx2] = ind;
            ext2_bwrite(&view2);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view2);
        if (ind == 0) {
            *out = 0;
            return 0;
        }

        struct ext2_block_view view3;
        ret = ext2_bread(ind, &view3);
        if (ret < 0)
            return ret;
        uint32 *ptr3 = (uint32 *)view3.data;
        uint32 blkno = ptr3[idx3];
        if (blkno == 0 && allocate) {
            blkno = ext2_alloc_block();
            if (blkno == 0) {
                ext2_brelse(&view3);
                return -ENOSPC;
            }
            ptr3[idx3] = blkno;
            ext2_bwrite(&view3);
            ext2_inode_blocks_add(info, 1);
        }
        ext2_brelse(&view3);
        *out = blkno;
        return 0;
    }

    return -EFBIG;
}

int ext2_readi(struct inode *inode, uint32 off, void *buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    if (off >= inode->size)
        return 0;

    struct ext2_inode_info *info = inode->private;
    uint32 bs = ext2_state.block_size;
    loff_t end = MIN((loff_t)off + len, inode->size);
    loff_t pos = off;

    while (pos < end) {
        uint32 lbn = (uint32)(pos / bs);
        uint32 block_off = (uint32)(pos % bs);
        uint32 to_read = MIN((uint32)(end - pos), bs - block_off);

        uint32 blkno = 0;
        int ret = ext2_get_block(info, lbn, 0, &blkno);
        if (ret < 0)
            return ret;

        if (blkno == 0) {
            vfs_either_copy_out(buf, ext2_zero_buf, to_read);
        } else {
            struct ext2_block_view view;
            ret = ext2_bread(blkno, &view);
            if (ret < 0)
                return ret;
            vfs_either_copy_out(buf, view.data + block_off, to_read);
            ext2_brelse(&view);
        }

        pos += to_read;
        buf = (void *)((uint64)buf + to_read);
    }

    return pos - off;
}

int ext2_writei(struct inode *inode, uint32 off, void *buf, loff_t len) {
    assert(holdingsleep(&inode->lock));

    struct ext2_inode_info *info = inode->private;
    uint32 bs = ext2_state.block_size;

    uint64 ptrs = bs / sizeof(uint32);
    uint64 max_blocks = (uint64)EXT2_NDIR_BLOCKS + ptrs + ptrs * ptrs + ptrs * ptrs * ptrs;
    uint64 max_size = max_blocks * bs;
    if ((uint64)off + (uint64)len > max_size)
        return -EFBIG;

    loff_t pos = off;
    loff_t end = off + len;
    int ret = 0;

    while (pos < end) {
        uint32 lbn = (uint32)(pos / bs);
        uint32 block_off = (uint32)(pos % bs);
        uint32 to_write = MIN((uint32)(end - pos), bs - block_off);

        uint32 blkno = 0;
        ret = ext2_get_block(info, lbn, 1, &blkno);
        if (ret < 0)
            break;

        struct ext2_block_view view;
        ret = ext2_bread(blkno, &view);
        if (ret < 0)
            break;
        vfs_either_copy_in(buf, view.data + block_off, to_write);
        ext2_bwrite(&view);
        ext2_brelse(&view);

        pos += to_write;
        buf = (void *)((uint64)buf + to_write);
    }

    if (pos > inode->size)
        inode->size = pos;

    info->inode.i_size = inode->size;
    info->inode.i_links_count = inode->nlinks;
    ext2_write_inode(info->ino, &info->inode);

    if (pos == off && ret < 0)
        return ret;
    return pos - off;
}

static void ext2_free_indirect(uint32 blkno, int level) {
    if (blkno == 0)
        return;

    struct ext2_block_view view;
    if (ext2_bread(blkno, &view) < 0)
        return;

    uint32 *ptr = (uint32 *)view.data;
    uint32 count = ext2_state.block_size / sizeof(uint32);

    for (uint32 i = 0; i < count; i++) {
        if (ptr[i] == 0)
            continue;
        if (level == 1) {
            ext2_free_block(ptr[i]);
        } else {
            ext2_free_indirect(ptr[i], level - 1);
        }
    }

    ext2_brelse(&view);
    ext2_free_block(blkno);
}

int ext2_truncate(struct inode *inode, uint32 new_size) {
    if (new_size != 0)
        return 0;

    struct ext2_inode_info *info = inode->private;

    for (int i = 0; i < EXT2_NDIR_BLOCKS; i++) {
        if (info->inode.i_block[i] != 0) {
            ext2_free_block(info->inode.i_block[i]);
            info->inode.i_block[i] = 0;
        }
    }

    if (info->inode.i_block[EXT2_IND_BLOCK]) {
        ext2_free_indirect(info->inode.i_block[EXT2_IND_BLOCK], 1);
        info->inode.i_block[EXT2_IND_BLOCK] = 0;
    }
    if (info->inode.i_block[EXT2_DIND_BLOCK]) {
        ext2_free_indirect(info->inode.i_block[EXT2_DIND_BLOCK], 2);
        info->inode.i_block[EXT2_DIND_BLOCK] = 0;
    }
    if (info->inode.i_block[EXT2_TIND_BLOCK]) {
        ext2_free_indirect(info->inode.i_block[EXT2_TIND_BLOCK], 3);
        info->inode.i_block[EXT2_TIND_BLOCK] = 0;
    }

    info->inode.i_size = 0;
    info->inode.i_blocks = 0;
    inode->size = 0;
    return 0;
}
