#include "debug.h"
#include "defs.h"
#include "fs.h"
#include "log.h"

static char *skipelem(char *path, char *name);

static struct inode *dlookupx(char *path, char *name, int stop_at_parent) {
    assert(curr_proc() != NULL);
    // this function must be called in a syscall context.

    struct dentry dentry;
    struct inode *ind;

    if (*path == '/') {
        ind = rootfs->root;
        iget(ind);
    } else {
        // cwd is only changed in syscall context.
        //  , so no need to acquire the plock.
        ind = curr_proc()->cwd;
        iget(ind);
    }
    assert(ind != NULL);
    assert(ind->ref >= 1);

    ilock(ind);
    while ((path = skipelem(path, name)) != 0) {
        if (!(ind->imode & IMODE_DIR))
            goto err;
        if (stop_at_parent && *path == '\0')
            goto out;
        if (strncmp(name, ".", DIRSIZ) == 0)
            continue;
        assert(ind->iops->lookup);

        strncpy(dentry.name, name, DIRSIZ);
        if (ind->iops->lookup(ind, &dentry) != 0)
            goto err;
        if (dentry.ind == ind) {
            iput(dentry.ind);
            continue;
        }

        // we are dropping the ind ptr, so iput
        iunlockput(ind);

        ind = dentry.ind;
        // dentry.ind is locked by the `lookup`.
    }
out:
    // when we are holding a pointer, we must hold a reference count.
    // return with a locked & get inode
    return ind;

err:
    iunlockput(ind);
    return NULL;
}

struct inode *dlookup(char *path) {
    char buf[DIRSIZ];
    return dlookupx(path, buf, 0);
}

struct inode *dlookup_parent(char *path, char *name) {
    return dlookupx(path, name, 1);
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
    char *s;
    int len;

    while (*path == '/') path++;
    if (*path == 0)
        return 0;
    s = path;
    while (*path != '/' && *path != 0) path++;
    len = path - s;
    if (len >= DIRSIZ)
        memmove(name, s, DIRSIZ);
    else {
        memmove(name, s, len);
        name[len] = 0;
    }
    while (*path == '/') path++;
    return path;
}
