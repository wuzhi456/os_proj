#include "defs.h"
#include "../fs/fs.h"

// block size: 4096

/***
 * On-Disk Data Structure
 *  - superblock
 *  - inode
 *  - bitmap
 * 
 *  Layout: (as blk) 
 *  | 0th superblock |
 *  | bitmap of inodes (x blks) |
 *  | bitmap of blocks (y blks) |
 *  | inodes (n blks)   |
 *  | blocks (n blks)   | 
 * 
 *  | log (not included) |
 */

// * blkno 0: | sfs_superblock | padding |
struct sfs_dsuperblock {
    uint32 magic;
    uint32 size;        // how many blocks in the whole fs structure? include this.
    uint32 nblocks;     // how many data blocks?
    uint32 ninodes;     // how many inodes?
    uint32 ind_bmap_starts;  // where does the bitmap of inode start?
    uint32 ind_bmap_count;  // where does the bitmap of inode start?
    uint32 blk_bmap_starts;  // where does the bitmap of inode start?
    uint32 blk_bmap_count;  // where does the bitmap of inode start?
    uint32 inodestart; // where the inodes starts.
    uint32 blockstart;  // where the blocks starts.
};
#define SFS_MAGIC 0x10203040

// In-memory superblock
struct sfs_vfs_superblock {
    struct sfs_dsuperblock __dsb;
    const struct sfs_dsuperblock* dsb;      // a const pointer
};

// On-Disk inode structure

#define NDIRECT 12
#define NINDIRECT (BSIZE / sizeof(uint32))
#define MAX_BLOCKS (NDIRECT + NINDIRECT)

struct sfs_dinode {
    uint16 type;
    uint16 devno;   // device number, for I_DEV
    uint16 _pad;
    uint16 nlink;   // number of links to this inode
    uint32 size;    // size of this file (in bytes)
    uint32 direct[NDIRECT];
    uint32 indirect;
};  // size of the on-disk inode: 16 * uint32 = 64 bytes.
static_assert(sizeof(struct sfs_dinode) == 64);

#define SIMPLEFS_DIRSIZE 28
struct sfs_dirent {
    uint32 ino;
    char name[SIMPLEFS_DIRSIZE];
};  // size of the dirent: 32 bytes
static_assert(sizeof(struct sfs_dirent) == 32);

// shared function to manipulate blkno/ino conversion:

// Inodes per block
#define IPB (BSIZE / sizeof(struct sfs_dinode))

// Inode offset in a block
#define IOFF(i) (i % IPB)

// Inode to disk block number conversion
static inline uint32 inode_to_disk_blkno(const struct sfs_dsuperblock* dsb, uint32 ino) {
    return (ino / IPB) + dsb->inodestart;
}

static inline struct sfs_dinode* blk_to_inode(void* blk, uint32 ino) {
    return &((struct sfs_dinode*)blk)[IOFF(ino)];
}

// datablock to disk block number conversion
static inline uint32 datablock_to_disk_blkno(const struct sfs_dsuperblock* dsb, uint32 blkno) {
    return (blkno) + dsb->blockstart;
}

// bitmap manipulation functions

#define BMAP_ENTRIES (BSIZE * 8)

#define BMAP_BLK_IDX(idx) ((idx) / BMAP_ENTRIES)
#define BMAP_BLK_OFF(idx) ((idx) % BMAP_ENTRIES)
#define BMAP_BYT_IDX(idx) ((idx) / 8)
#define BMAP_BIT_IDX(idx) ((idx) % 8)

static inline uint32 bmap_to_full_idx(uint32 blkidx, uint32 blkoff) {
    assert(blkoff < BMAP_ENTRIES);
    return (blkidx * BMAP_ENTRIES) + blkoff;
}

static inline void bmap_set(void* blk_bitmap, uint32 blkoff, int true_or_false) {
    assert(blkoff < BMAP_ENTRIES);

    uint8* byte = (uint8*)blk_bitmap + BMAP_BYT_IDX(blkoff);
    uint8 bit = 1 << BMAP_BIT_IDX(blkoff);
    if (true_or_false) {
        *byte |= bit;  // set the bit
    } else {
        *byte &= ~bit;  // clear the bit
    }
}

static inline int bmap_get(void* blk_bitmap, uint32 blkoff) {
    assert(blkoff < BMAP_ENTRIES);

    uint8* byte = (uint8*)blk_bitmap + BMAP_BYT_IDX(blkoff);
    uint8 bit = 1 << BMAP_BIT_IDX(blkoff);
    return (*byte & bit) != 0;  // return true if the bit is set
}

/**
 * @brief Find a free bit in the bitmap and set it. Return the in-block index
 * 
 * @param blk_bitmap 
 * @return int32 
 */
static inline uint32 bmap_find_first_zero(void* blk_bitmap) {
    uint32 off = 0;
    uint8* byte = (uint8*)blk_bitmap;
    while (off < BMAP_ENTRIES) {
        if (*byte == 0xFF) {
            // all bits are set, move to the next byte
            byte++;
            off += 8;
            continue;
        } else {
            // if not, there must be a free bit.
            for (int i = 0; i < 8; i++) {
                if (!(*byte & (1 << i))) {
                    // set the bit
                    *byte |= (1 << i);
                    return off + i;  // return the index of the first zero bit
                }
            }
            assert(0 && "should not reach here, all bits are set");
        }
    }
    return BMAP_ENTRIES; // not found
}

// sfs functions:
void sfs_vfs_init(void);

void sfs_init(void);
uint32 balloc();
void bfree(uint32 blkno);
uint32 ialloc();
void ifree(uint32 ino);
void iupdate(struct inode* inode);
int iaddr(struct inode* inode, uint32 addr, uint32* oblkno);
int iextend(struct inode* inode, uint32 addr);
int iread(struct inode* inode, uint32 addr, void* __either buf, loff_t len);
int iwrite(struct inode* inode, uint32 addr, void* __either buf, loff_t len);
void izero(uint32 ino);