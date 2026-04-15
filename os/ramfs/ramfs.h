#include "../fs/fs.h"
#include "defs.h"

struct ramfs_superblock {
    struct ramfs_inode* root;
    uint32 used_ino;
    spinlock_t lock;
};

struct dir_content {
    uint32 ino;
    char name[28];
} __attribute__((packed));

struct ramfs_inode {
    uint32 ino;
    imode_t imode;
    loff_t size;
    int nlinks;
    struct ramfs_inode* next;
    union {
        void* filedata;
        struct dir_content* dirs;
    };
};

#define MAX_DIRS (PGSIZE / sizeof(struct dir_content))

// ramfs manipulation
struct ramfs_inode* ramfs_create_inode(struct ramfs_superblock* sb, struct ramfs_inode* dir, char* name, imode_t imode);


static void ramfs_inos_insert(struct ramfs_superblock* sb, struct ramfs_inode* ind) {
    acquire(&sb->lock);
    ind->next = sb->root;
    sb->root  = ind;
    release(&sb->lock);
}

static void ramfs_inos_remove(struct ramfs_superblock* sb, struct ramfs_inode* ind) {
    acquire(&sb->lock);
    struct ramfs_inode* cur = sb->root;
    while (cur->next != NULL) {
        if (cur->next == ind) {
            cur->next = ind->next;
            goto out;
        }
        cur = cur->next;
    }
    out:
    release(&sb->lock);
}

static struct ramfs_inode* ramfs_inos_find(struct ramfs_superblock* sb, uint32 ino) {
    acquire(&sb->lock);
    struct ramfs_inode* cur = sb->root;
    while (cur != NULL) {
        if (cur->ino == ino) {
            release(&sb->lock);
            return cur;
        }
        cur = cur->next;
    }
    release(&sb->lock);
    return NULL;
}

static uint32 ramfs_next_ino(struct ramfs_superblock* sb) {
    acquire(&sb->lock);
    uint32 ino = sb->used_ino++;
    release(&sb->lock);
    return ino;
}