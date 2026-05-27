#ifndef EXT2_H
#define EXT2_H

#include "types.h"
#include "lock.h"
#include "fs/fs.h"

#define EXT2_SUPER_MAGIC 0xEF53

#define EXT2_S_IFIFO 0x1000
#define EXT2_S_IFDIR 0x4000
#define EXT2_S_IFREG 0x8000

#define EXT2_NDIR_BLOCKS 12
#define EXT2_IND_BLOCK   12
#define EXT2_DIND_BLOCK  13
#define EXT2_TIND_BLOCK  14
#define EXT2_N_BLOCKS    15

#define EXT2_FT_UNKNOWN  0
#define EXT2_FT_REG_FILE 1
#define EXT2_FT_DIR      2
#define EXT2_FT_FIFO     6

struct ext2_superblock {
    uint32 s_inodes_count;
    uint32 s_blocks_count;
    uint32 s_r_blocks_count;
    uint32 s_free_blocks_count;
    uint32 s_free_inodes_count;
    uint32 s_first_data_block;
    uint32 s_log_block_size;
    uint32 s_log_frag_size;
    uint32 s_blocks_per_group;
    uint32 s_frags_per_group;
    uint32 s_inodes_per_group;
    uint32 s_mtime;
    uint32 s_wtime;
    uint16 s_mnt_count;
    uint16 s_max_mnt_count;
    uint16 s_magic;
    uint16 s_state;
    uint16 s_errors;
    uint16 s_minor_rev_level;
    uint32 s_lastcheck;
    uint32 s_checkinterval;
    uint32 s_creator_os;
    uint32 s_rev_level;
    uint16 s_def_resuid;
    uint16 s_def_resgid;

    uint32 s_first_ino;
    uint16 s_inode_size;
    uint16 s_block_group_nr;
    uint32 s_feature_compat;
    uint32 s_feature_incompat;
    uint32 s_feature_ro_compat;
    uint8 s_uuid[16];
    char s_volume_name[16];
    char s_last_mounted[64];
    uint32 s_algorithm_usage_bitmap;

    uint8 s_reserved[820];
} __attribute__((packed));

struct ext2_group_desc {
    uint32 bg_block_bitmap;
    uint32 bg_inode_bitmap;
    uint32 bg_inode_table;
    uint16 bg_free_blocks_count;
    uint16 bg_free_inodes_count;
    uint16 bg_used_dirs_count;
    uint16 bg_pad;
    uint32 bg_reserved[3];
} __attribute__((packed));

struct ext2_inode {
    uint16 i_mode;
    uint16 i_uid;
    uint32 i_size;
    uint32 i_atime;
    uint32 i_ctime;
    uint32 i_mtime;
    uint32 i_dtime;
    uint16 i_gid;
    uint16 i_links_count;
    uint32 i_blocks;
    uint32 i_flags;
    uint32 i_osd1;
    uint32 i_block[EXT2_N_BLOCKS];
    uint32 i_generation;
    uint32 i_file_acl;
    uint32 i_dir_acl;
    uint32 i_faddr;
    uint8 i_osd2[12];
} __attribute__((packed));

struct ext2_dir_entry {
    uint32 inode;
    uint16 rec_len;
    uint8 name_len;
    uint8 file_type;
    char name[];
} __attribute__((packed));

struct ext2_fs {
    struct ext2_superblock sb;
    uint32 block_size;
    uint32 group_count;
    struct ext2_group_desc *groups;
    sleeplock_t lock;
};

enum ext2_journal_state {
    EXT2_JOURNAL_EMPTY    = 0,
    EXT2_JOURNAL_PREPARED = 1,
    EXT2_JOURNAL_COMMITTED = 2,
};

struct ext2_journal_hdr {
    uint32 magic;
    uint32 state;
    uint32 target_block;
    uint32 seq;
};

struct ext2_inode_info {
    struct ext2_inode inode;
    uint32 ino;
    void *fifo;
};

int ext2_probe(void);
int ext2_init(void);
int ext2_vfs_init(void);
int ext2_journal_init(struct inode *journal_inode);

struct ext2_fs *ext2_get_fs(void);

int ext2_read_inode(uint32 ino, struct ext2_inode *out);
int ext2_write_inode(uint32 ino, struct ext2_inode *in);
uint32 ext2_alloc_block(void);
void ext2_free_block(uint32 blkno);
uint32 ext2_alloc_inode(void);
void ext2_free_inode(uint32 ino);
int ext2_readi(struct inode *inode, uint32 off, void *buf, loff_t len);
int ext2_writei(struct inode *inode, uint32 off, void *buf, loff_t len);
int ext2_truncate(struct inode *inode, uint32 new_size);

#endif
