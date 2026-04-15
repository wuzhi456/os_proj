#include "ramfs.h"
#include "../fs/fs.h"
#include "defs.h"

struct ramfs_superblock ramfs_sb;
allocator_t allocator_ramfsinode;

void ramfs_prepare(struct ramfs_superblock* sb) {
    allocator_init(&allocator_ramfsinode, "ramfsinode", sizeof(struct ramfs_inode), 1024);
    sb->root = NULL;
    sb->used_ino = 0;
    spinlock_init(&sb->lock, "ramfslock");

    // Step.1 Create the root directory 

    struct ramfs_inode* root = (struct ramfs_inode*)kalloc(&allocator_ramfsinode);

    root->ino      = ramfs_next_ino(sb);
    root->size     = 0;
    root->next     = NULL;
    root->filedata = (void*)PA_TO_KVA(kallocpage());
    root->imode    = IMODE_DIR;

    ramfs_inos_insert(sb, root);
    ramfs_create_inode(sb, root, ".", IMODE_DIR);

    // Step.2 Create a file named "hello" in the root directory
    ramfs_create_inode(sb, root, "hello", IMODE_REG);
}

struct ramfs_inode* ramfs_create_inode(struct ramfs_superblock* sb, struct ramfs_inode* dir, char* name, imode_t imode) {
    if (dir->size >= MAX_DIRS)
        return NULL;

    struct ramfs_inode* ind     = (struct ramfs_inode*)kalloc(&allocator_ramfsinode);

    ind->ino      = ramfs_next_ino(sb);
    ind->size     = 0;
    ind->next     = NULL;
    ind->filedata = (void*)PA_TO_KVA(kallocpage());
    ind->imode    = imode;

    ramfs_inos_insert(sb, ind);

    struct dir_content* dirent = &dir->dirs[dir->size++];
    dirent->ino = ind->ino;
    strncpy(dirent->name, name, DIRSIZ);
    
    return ind;
}