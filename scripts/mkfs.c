#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <stdint.h>
#include <sys/mman.h>


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
struct sfs_dsuperblock {
    uint32_t magic;
    uint32_t size;        // how many blocks in the whole fs structure? include this.
    uint32_t nblocks;     // how many data blocks?
    uint32_t ninodes;     // how many inodes?
    uint32_t ind_bmap_starts;  // where does the bitmap of inode start?
    uint32_t ind_bmap_count;  // where does the bitmap of inode start?
    uint32_t blk_bmap_starts;  // where does the bitmap of inode start?
    uint32_t blk_bmap_count;  // where does the bitmap of inode start?
    uint32_t inodestart; // where the inodes starts.
    uint32_t blockstart;  // where the blocks starts.
};

#define NDIRECT 12
struct sfs_dinode {
    uint16_t type;
    uint16_t devno;   // device number, for I_DEV
    uint16_t _pad;
    uint16_t nlink;   // number of links to this inode
    uint32_t size;    // size of this file (in bytes)
    uint32_t direct[NDIRECT];
    uint32_t indirect;
};  // size of the on-disk inode: 16 * uint32_t = 64 bytes.
static_assert(sizeof(struct sfs_dinode) == 64, "qwq");

#define SIMPLEFS_DIRSIZE 28
struct sfs_dirent {
    uint32_t ino;
    char name[SIMPLEFS_DIRSIZE];
};  // size of the dirent: 32 bytes
static_assert(sizeof(struct sfs_dirent) == 32);

#define ROUNDUP_2N(x, n) (((x) + (n) - 1) & ~((n) - 1))

#define SFS_MAGIC 0x10203040
#define BSIZE 4096  // block size

#define IMODE_DEVICE 0x100
#define IMODE_REG    0x200
#define IMODE_DIR    0x400

// bitmap

#define BMAP_ENTRIES (BSIZE * 8)
#define BMAP_BLK_IDX(idx) ((idx) / BMAP_ENTRIES)
#define BMAP_BLK_OFF(idx) ((idx) % BMAP_ENTRIES)
#define BMAP_BYT_IDX(idx) ((idx) / 8)
#define BMAP_BIT_IDX(idx) ((idx) % 8)

static inline void bmap_set(void* blk_bitmap, uint32_t blkoff, int true_or_false) {
    assert(blkoff < BMAP_ENTRIES);

    uint8_t* byte = (uint8_t*)blk_bitmap + BMAP_BYT_IDX(blkoff);
    uint8_t bit = 1 << BMAP_BIT_IDX(blkoff);
    if (true_or_false) {
        *byte |= bit;  // set the bit
    } else {
        *byte &= ~bit;  // clear the bit
    }
}

uint32_t ialloc(struct sfs_dsuperblock* dsb, void* ptr) {
    static uint32_t ino = 0;
    if (ino >= dsb->ninodes) {
        fprintf(stderr, "No more inodes available\n");
        exit(EXIT_FAILURE);
    }
    // mark 1 in bitmap
    bmap_set((uint8_t*)ptr + (dsb->ind_bmap_starts + BMAP_BLK_IDX(ino)) * BSIZE, BMAP_BLK_OFF(ino), 1);
    return ino++;
}

uint32_t balloc(struct sfs_dsuperblock* dsb, void* ptr) {
    static uint32_t bno = 0;
    if (bno >= dsb->nblocks) {
        fprintf(stderr, "No more inodes available\n");
        exit(EXIT_FAILURE);
    }
    // mark 1 in bitmap
    bmap_set((uint8_t*)ptr + (dsb->blk_bmap_starts + BMAP_BLK_IDX(bno)) * BSIZE, BMAP_BLK_OFF(bno), 1);
    return bno++;
}

static void usage() {
    fprintf(stderr, "Usage: mkfs <file> [# of inodes] [# of blocks]\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char**argv) {
    if (argc != 4) {
        usage();
    }
    const char* filename = argv[1];
    int ninodes = atoi(argv[2]);
    int nblocks = atoi(argv[3]);

    int nblocks_inode = ROUNDUP_2N(ninodes * sizeof(struct sfs_dinode), BSIZE) / BSIZE;

    int nblocks_bmap_inode = ROUNDUP_2N(ninodes, BMAP_ENTRIES) / BMAP_ENTRIES;
    int nblocks_bmap_datab = ROUNDUP_2N(nblocks, BMAP_ENTRIES) / BMAP_ENTRIES;
    int nblocks_total = 1 + nblocks_bmap_inode + nblocks_bmap_datab + nblocks_inode + nblocks;

    printf("Creating filesystem: \n");
    printf("    - # of inodes: %d \n", ninodes);
    printf("    - # of blocks: %d \n", nblocks);
    printf("    - # of bitmap block for inodes: %d \n", nblocks_bmap_inode);
    printf("    - # of bitmap block for blocks: %d \n", nblocks_bmap_datab);
    printf("    - Total Blocks: %d, Size (hex): %x \n", nblocks_total, nblocks_total * BSIZE);

    int fd = open(filename, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        exit(EXIT_FAILURE);
    }
    if (ftruncate(fd, nblocks_total * BSIZE) < 0) {
        perror("ftruncate");
        close(fd);
        exit(EXIT_FAILURE);
    }
    lseek(fd, 0, SEEK_SET);  // ensure we are at the beginning of the file
    // mmap the file.
    void* mmap_ptr = mmap(NULL, nblocks_total * BSIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmap_ptr == MAP_FAILED) {
        perror("mmap");
        close(fd);
        exit(EXIT_FAILURE);
    }
    close(fd);  // close the file descriptor, we don't need it anymore

    struct sfs_dsuperblock* dsb = (struct sfs_dsuperblock*)mmap_ptr;
    dsb->magic = SFS_MAGIC;
    dsb->size = nblocks_total;
    dsb->nblocks = nblocks;
    dsb->ninodes = ninodes;
    dsb->ind_bmap_starts = 1;  // start after the superblock
    dsb->ind_bmap_count = nblocks_bmap_inode;
    dsb->blk_bmap_starts = dsb->ind_bmap_starts + nblocks_bmap_inode;
    dsb->blk_bmap_count = nblocks_bmap_datab;
    dsb->inodestart = dsb->blk_bmap_starts + nblocks_bmap_datab;
    dsb->blockstart = dsb->inodestart + nblocks_inode;
    printf("Superblock initialized at %p\n", dsb);

    // set all bitmap to 1
    memset((uint8_t*)mmap_ptr + dsb->ind_bmap_starts * BSIZE, 0xFF, nblocks_bmap_inode * BSIZE);
    memset((uint8_t*)mmap_ptr + dsb->blk_bmap_starts * BSIZE, 0xFF, nblocks_bmap_datab * BSIZE);

    // then set 0 - nblocks to 0
    for (int i = 0; i < nblocks; i++) {
        bmap_set((uint8_t*)mmap_ptr + (dsb->blk_bmap_starts + BMAP_BLK_IDX(i)) * BSIZE, BMAP_BLK_OFF(i), 0);
    }

    // then set 0 - ninodes to 0
    for (int i = 0; i < ninodes; i++) {
        bmap_set((uint8_t*)mmap_ptr + (dsb->ind_bmap_starts + BMAP_BLK_IDX(i)) * BSIZE, BMAP_BLK_OFF(i), 0);
    }

    // root inode: 0th
    uint32_t root_ino = ialloc(dsb, mmap_ptr);
    assert(root_ino == 0);  // root inode is always 0
    struct sfs_dinode* root_inode = &((struct sfs_dinode*)((uint8_t*)mmap_ptr + dsb->inodestart * BSIZE))[0];
    memset(root_inode, 0, sizeof(struct sfs_dinode));
    root_inode->type = IMODE_DIR;  // root is a directory
    root_inode->nlink = 1;  // root has one link
    printf("Root inode initialized at %p\n", root_inode);
    
    uint32_t blk0th = balloc(dsb, mmap_ptr);
    assert(blk0th == 0);
    // 0th block is used to describe "uninitialized" state, so we occupy it in the bitmap.

    uint32_t root_block = balloc(dsb, mmap_ptr);
    assert(root_block == 1);  // root block is always 1

    struct sfs_dirent* root_dirent = (struct sfs_dirent*)((uint8_t*)mmap_ptr + (dsb->blockstart + root_block) * BSIZE);
    root_dirent->ino = root_ino;  // root inode number
    strncpy(root_dirent->name, ".", SIMPLEFS_DIRSIZE - 1);  // root directory name is "."
    printf("Root directory initialized at %p\n", root_dirent);

    root_inode->size += sizeof(struct sfs_dirent);  // increase size of root inode
    root_inode->direct[0] = root_block;  // set the first direct block to the root block

    // create the first file "hello"
    struct sfs_dirent* root_dirent1 = root_dirent + 1;
    root_dirent1->ino = ialloc(dsb, mmap_ptr);
    strncpy(root_dirent1->name, "hello", SIMPLEFS_DIRSIZE - 1);
    root_inode->size += sizeof(struct sfs_dirent);  // increase size of root inode


    struct sfs_dinode* hello_ind = &((struct sfs_dinode*)((uint8_t*)mmap_ptr + dsb->inodestart * BSIZE))[root_dirent1->ino];
    memset(hello_ind, 0, sizeof(struct sfs_dinode));
    hello_ind->type = IMODE_REG;  // hello is a regular file
    hello_ind->nlink = 1;  // hello has one link
    hello_ind->size = 0;  // hello is empty
    hello_ind->direct[0] = balloc(dsb, mmap_ptr);  // allocate a block for hello
    assert(hello_ind->direct[0] == 2);  // hello's first block is always 2

    
    // unmmap and close
    if (munmap(mmap_ptr, nblocks_total * BSIZE) < 0) {
        perror("munmap");
        close(fd);
        exit(EXIT_FAILURE);
    }
    printf("Filesystem created successfully at %s\n", filename);    
}
