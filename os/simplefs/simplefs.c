#include "simplefs.h"

#include "../fs/buf.h"

struct sfs_vfs_superblock sb;

static void bzero(uint32 blkno);

void sfs_init(void) {
    memset(&sb, 0, sizeof(sb));
    sb.dsb = &sb.__dsb;

    struct buf* bp;
    bp = bread(0, 0);
    memmove(&sb.__dsb, bp->data, sizeof(sb.__dsb));
    brelse(bp);

    // sanity check
    assert(sb.dsb->magic == SFS_MAGIC);

    uint32 max_inodes_expressed_by_bmap = sb.dsb->ind_bmap_count * BMAP_ENTRIES;
    assert(sb.dsb->ninodes <= max_inodes_expressed_by_bmap);

    uint32 max_blocks_expressed_by_bmap = sb.dsb->blk_bmap_count * BMAP_ENTRIES;
    assert(sb.dsb->nblocks <= max_blocks_expressed_by_bmap);
}

//  Block and Inode Allocation and Deallocation

/**
 * @brief Allocate a new block, and zero it.
 *
 * @return the block number if successful, 0 if fails.
 */
uint32 balloc() {
    for (uint i = 0; i < sb.dsb->blk_bmap_count; i++) {
        struct buf* bmap = bread(0, sb.dsb->blk_bmap_starts + i);
        uint32 bmapoff   = bmap_find_first_zero(bmap->data);

        if (bmapoff == BMAP_ENTRIES) {
            // full in the first bitmap block, continue to the next one
            brelse(bmap);
            continue;
        }

        // we found a free block, and we have written the bit in `find_first_zero`
        bwrite(bmap);
        brelse(bmap);

        uint32 blkno = bmap_to_full_idx(i, bmapoff);
        bzero(blkno);  // zero the block
        return blkno;
    }
    return 0;  // no free block found
}

/**
 * @brief Free a block in the bitmap.
 *
 * @param blkno
 */
void bfree(uint32 blkno) {
    assert(blkno > 0);  // 0th block is always occupied by superblock
    uint32 bmap_no  = BMAP_BLK_IDX(blkno);
    uint32 bmap_off = BMAP_BLK_OFF(blkno);

    struct buf* bmap = bread(0, sb.dsb->blk_bmap_starts + bmap_no);
    bmap_set(bmap->data, bmap_off, 0);  // clear the bit
    bwrite(bmap);
    brelse(bmap);
}

/**
 * @brief Allocate a new inode, and zero it.
 *
 * @return uint32 the inode number if successful, 0 if fails.
 */
uint32 ialloc() {
    for (uint i = 0; i < sb.dsb->ind_bmap_count; i++) {
        struct buf* bmap = bread(0, sb.dsb->ind_bmap_starts + i);
        uint32 bmapoff   = bmap_find_first_zero(bmap->data);

        if (bmapoff == BMAP_ENTRIES) {
            // full in the first bitmap block, continue to the next one
            brelse(bmap);
            continue;
        }

        // we found a free block, and we have written the bit in `find_first_zero`
        bwrite(bmap);
        brelse(bmap);

        uint32 ino = bmap_to_full_idx(i, bmapoff);
        return ino;
    }
    return 0;  // no free inode found
}

/**
 * @brief Free an inode in the bitmap.
 *
 * @param ino
 */
void ifree(uint32 ino) {
    assert(ino > 0);  // 0th block is always occupied by superblock

    izero(ino);  // zero the inode data

    uint32 bmap_no  = BMAP_BLK_IDX(ino);
    uint32 bmap_off = BMAP_BLK_OFF(ino);

    struct buf* bmap = bread(0, sb.dsb->ind_bmap_starts + bmap_no);
    bmap_set(bmap->data, bmap_off, 0);  // clear the bit
    bwrite(bmap);
    brelse(bmap);
}

// Inode Manipulation

/**
 * @brief Sync a modified in-memory VFS inode to disk. Caller must hold ip->lock.
 *
 * @param inode A VFS inode.
 */
void iupdate(struct inode* inode) {
    assert(holdingsleep(&inode->lock));
    struct buf* bp                = bread(0, inode_to_disk_blkno(sb.dsb, inode->ino));
    struct sfs_dinode* disk_inode = blk_to_inode(bp->data, inode->ino);
    disk_inode->size              = inode->size;
    disk_inode->nlink             = inode->nlinks;
    bwrite(bp);
    brelse(bp);
    // Note: we do not update imode, as it is immutable after allocation.
}

/**
 * @brief Get the data block blkno at the given address in the inode.
 *
 * @param inode
 * @param addr in bytes
 * @param blkno output parameter to store the blkno.
 * @return 0 if successful, negative error code if failed.
 */
int iaddr(struct inode* inode, uint32 addr, uint32* oblkno) {
    assert(holdingsleep(&inode->lock));

    uint32 addr_blkno = addr / BSIZE;
    if (addr_blkno >= MAX_BLOCKS) {
        return -EINVAL;  // address out of range
    }

    int ret = 0;

    struct buf* inode_buf            = bread(0, inode_to_disk_blkno(sb.dsb, inode->ino));
    struct sfs_dinode* disk_inode = blk_to_inode(inode_buf->data, inode->ino);

    if (addr_blkno < NDIRECT) {
        // direct block
        uint32 blkno = disk_inode->direct[addr_blkno];
        if (blkno == 0) {
            blkno = balloc();  // allocate a new block, zero-ed
            if (blkno == 0) {
                ret = -ENOSPC;  // no space left on device
                goto out;
            }
            disk_inode->direct[addr_blkno] = blkno;
            bwrite(inode_buf);  // write back the inode block
        }
        *oblkno = blkno;
    } else {
        // indirect block
        uint32 indirect_blkno = disk_inode->indirect;
        if (indirect_blkno == 0) {
            // allocate the indirect block
            indirect_blkno = balloc();  // allocate a new block, zero-ed
            if (indirect_blkno == 0) {
                ret = -ENOSPC;  // no space left on device
                goto out;
            }
            disk_inode->indirect = indirect_blkno;
            bwrite(inode_buf);  // write back the inode block
        }

        struct buf* ind_buf = bread(0, datablock_to_disk_blkno(sb.dsb, indirect_blkno));
        uint32* ind_data    = (uint32*)ind_buf->data;

        addr_blkno = addr_blkno - NDIRECT;  // convert to the index inside the indirect block
        uint32 blkno = ind_data[addr_blkno];

        if (blkno == 0) {
            blkno = balloc();  // allocate a new block, zero-ed
            if (blkno == 0) {
                ret = -ENOSPC;  // no space left on device
                brelse(ind_buf);
                goto out;
            }
            ind_data[addr_blkno] = blkno;
            bwrite(ind_buf);  // write back the indirect block
        }
        brelse(ind_buf);
        *oblkno = blkno;
    }

out:
    brelse(inode_buf);
    return ret;
}

int iextend(struct inode* inode, uint32 addr) {
    if (addr >= MAX_BLOCKS * BSIZE) {
        return -EINVAL;  // extend out of range
    }
    int ret = 0;

    uint32 pos = inode->size / BSIZE;
    uint32 end = ROUNDUP_2N(addr, BSIZE) / BSIZE;  // round up to the next block

    while (pos < end) {
        uint32 blkno;
        ret = iaddr(inode, pos * BSIZE, &blkno);
        if (ret != 0)
            goto out;
        pos++;
    }
    inode->size = addr;

out:
    iupdate(inode);
    return ret;
}

int iread(struct inode* inode, uint32 addr, void* __either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    if (addr >= inode->size) {
        return 0;  // read out of range
    }

    int ret = 0;
    loff_t pos = addr;
    loff_t end = MIN(addr + len, inode->size);

    while (pos < end) {
        uint32 blkno;
        ret = iaddr(inode, pos, &blkno);
        if (ret < 0) 
            goto out;
        struct buf* bp = bread(0, datablock_to_disk_blkno(sb.dsb, blkno));

        // calculate pos & len within the block
        uint32 offset = pos % BSIZE;
        uint32 to_read = MIN(end - pos, BSIZE - offset);
        vfs_either_copy_out(buf, bp->data + offset, to_read);

        brelse(bp);
        pos += to_read;
        buf = (void*)((uint64)buf + to_read);  // move the buffer pointer
    }

out:
    return pos - addr;  // return the number of bytes read
}

int iwrite(struct inode* inode, uint32 addr, void* __either buf, loff_t len) {
    assert(holdingsleep(&inode->lock));
    if (addr >= MAX_BLOCKS * BSIZE) {
        return -EFBIG;  // write out of range
    }
    int ret = 0;

    loff_t pos = addr;
    loff_t end = MIN(addr + len, MAX_BLOCKS * BSIZE);

    while (pos < end) {
        uint32 blkno;
        ret = iaddr(inode, pos, &blkno);
        if (ret < 0) 
            goto out;
        struct buf* bp = bread(0, datablock_to_disk_blkno(sb.dsb, blkno));

        // calculate pos & len within the block
        uint32 offset = pos % BSIZE;
        uint32 to_read = MIN(end - pos, BSIZE - offset);
        vfs_either_copy_in(buf, bp->data + offset, to_read);
        bwrite(bp);
        brelse(bp);

        pos += to_read;
        buf = (void*)((uint64)buf + to_read);  // move the buffer pointer
    }

out:
    if (pos > inode->size) {
        inode->size = pos;  // update the size if we wrote beyond the current size
        iupdate(inode);
    }
    // return the number of bytes written
    if (pos == addr && ret < 0)
        return ret;  // if we didn't write anything, return the error code
    return pos - addr;  
}

void bzero(uint32 blkno) {
    struct buf* bp = bread(0, sb.dsb->blockstart + blkno);
    memset(bp->data, 0, BSIZE);
    bwrite(bp);
    brelse(bp);
}

void izero(uint32 ino) {
    struct buf* bp                = bread(0, inode_to_disk_blkno(sb.dsb, ino));
    struct sfs_dinode* disk_inode = blk_to_inode(bp->data, ino);
    memset(disk_inode, 0, sizeof(*disk_inode));
    bwrite(bp);
    brelse(bp);
}