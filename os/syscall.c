#include "syscall.h"

#include "console.h"
#include "defs.h"
#include "fs/fs.h"
#include "ktest/ktest.h"
#include "loader.h"
#include "timer.h"
#include "trap.h"

static int fdalloc(struct file *f) {
    int fd;
    struct proc *p = curr_proc();

    for (fd = 0; fd < NPROCFILE; fd++) {
        if (p->fdtable[fd] == NULL) {
            p->fdtable[fd] = f;
            return fd;
        }
    }
    return -1;
}

static struct file *fdget(int fd) {
    if (fd < 0 || fd >= NPROCFILE)
        return NULL;
    return curr_proc()->fdtable[fd];
}

static int fetch_path(uint64 __user upath, char **out) {
    int ret;
    char *path = kalloc(&kstrbuf);
    if (path == NULL)
        return -ENOMEM;

    memset(path, 0, KSTRING_MAX);
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    ret = copystr_from_user(p->mm, path, upath, KSTRING_MAX);
    release(&p->mm->lock);
    if (ret < 0) {
        kfree(&kstrbuf, path);
        return ret;
    }

    *out = path;
    return 0;
}

static void fill_stat_locked(struct inode *inode, struct stat *st) {
    assert(holdingsleep(&inode->lock));
    memset(st, 0, sizeof(*st));
    st->ino    = inode->ino;
    st->mode   = inode->imode;
    st->nlinks = inode->nlinks;
    st->size   = inode->size;
}

static int copy_stat_to_user(uint64 __user ust, struct stat *kst) {
    int ret;
    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    ret = copy_to_user(p->mm, ust, (char *)kst, sizeof(*kst));
    release(&p->mm->lock);
    return ret;
}

int64 sys_fork() {
    return fork();
}

int64 sys_exec(uint64 __user path, uint64 __user argv) {
    int ret;
    char *kpath = kalloc(&kstrbuf);
    char *arg[MAXARG];
    memset(kpath, 0, KSTRING_MAX);
    memset(arg, 0, sizeof(arg));

    struct proc *p = curr_proc();

    acquire(&p->lock);
    acquire(&p->mm->lock);
    release(&p->lock);

    if ((ret = copystr_from_user(p->mm, kpath, path, KSTRING_MAX)) < 0) {
        goto free;
    }
    for (int i = 0; i < MAXARG; i++) {
        uint64 useraddr;
        if ((ret = copy_from_user(p->mm, (char *)&useraddr, argv + i * sizeof(uint64), sizeof(uint64))) < 0) {
            goto free;
        }
        if (useraddr == 0) {
            arg[i] = 0;
            break;
        }
        arg[i] = kalloc(&kstrbuf);
        assert(arg[i] != NULL);
        if ((ret = copystr_from_user(p->mm, arg[i], useraddr, KSTRING_MAX)) < 0) {
            goto free;
        }
    }
    release(&p->mm->lock);

    debugf("sys_exec %s\n", kpath);

    ret = exec(kpath, arg);

    kfree(&kstrbuf, kpath);
    for (int i = 0; arg[i]; i++) {
        kfree(&kstrbuf, arg[i]);
    }
    return ret;

free:
    release(&p->mm->lock);
    kfree(&kstrbuf, kpath);
    for (int i = 0; arg[i]; i++) {
        kfree(&kstrbuf, arg[i]);
    }
    return ret;
}

int64 sys_exit(int code) {
    exit(code);
    panic_never_reach();
}

int64 sys_wait(int pid, uint64 __user va) {
    struct proc *p = curr_proc();
    int *code      = NULL;

    return wait(pid, (int*) va);
}

int64 sys_getpid() {
    struct proc *cur = curr_proc();
    int pid;

    acquire(&cur->lock);
    pid = cur->pid;
    release(&cur->lock);

    return pid;
}

int64 sys_getppid() {
    struct proc *cur = curr_proc();
    int ppid;

    acquire(&cur->lock);
    ppid = cur->parent == NULL ? 0 : cur->parent->pid;
    release(&cur->lock);

    return ppid;
}

int64 sys_kill(int pid) {
    return kill(pid);
}

int64 sys_sleep(int64 n) {
    struct proc *p = curr_proc();

    acquire(&tickslock);
    uint64 ticks0 = ticks;
    while (ticks - ticks0 < n) {
        if (iskilled(p)) {
            release(&tickslock);
            return -1;
        }
        sleep(&ticks, &tickslock);
    }
    release(&tickslock);
    return 0;
}

int64 sys_yield() {
    yield();
    return 0;
}

int64 sys_sbrk(int64 n) {
    int64 ret;
    struct proc *p = curr_proc();

    acquire(&p->lock);
    acquire(&p->mm->lock);

    struct vma *vma_brk = p->vma_brk;
    int64 old_brk       = p->brk;
    int64 new_brk       = (int64)p->brk + n;

    if (new_brk < vma_brk->vm_start) {
        warnf("userprog requested to shrink brk, but underflow.");
        ret = -EINVAL;
    } else {
        int64 roundup = PGROUNDUP(new_brk);
        if (roundup == vma_brk->vm_end) {
            ret = 0;
        } else {
            ret = mm_remap(vma_brk, vma_brk->vm_start, roundup, vma_brk->pte_flags);
        }
        if (ret == 0) {
            p->brk = new_brk;
        }
    }

    release(&p->mm->lock);
    release(&p->lock);

    if (ret == 0) {
        return old_brk;
    }
    return ret;
}

int64 sys_mmap() {
    panic("unimplemented");
}

int64 sys_read(int fd, uint64 __user va, uint64 len) {
    struct file *f = fdget(fd);
    if (f == NULL) {
        return -EBADF;
    }
    return vfs_read(f, (char *)va, len);
}

int64 sys_write(int fd, uint64 __user va, uint len) {
    struct file *f = fdget(fd);
    if (f == NULL) {
        return -EBADF;
    }
    return vfs_write(f, (char *)va, len);
}

int64 sys_pipe(int __user fds[2]) {
    int ret;
    int fd0 = -1, fd1 = -1;
    int kfds[2];
    struct file *rf, *wf;
    struct proc *p = curr_proc();

    if (pipealloc(&rf, &wf) < 0) {
        return -ENOMEM;
    }

    if ((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0) {
        if (fd0 >= 0)
            p->fdtable[fd0] = NULL;
        fput(rf);
        fput(wf);
        return -EBADF;
    }

    kfds[0] = fd0;
    kfds[1] = fd1;

    acquire(&p->mm->lock);
    ret = copy_to_user(p->mm, (uint64)fds, (char *)&kfds, sizeof(kfds));
    release(&p->mm->lock);
    if (ret < 0) {
        fput(rf);
        fput(wf);
        p->fdtable[fd0] = NULL;
        p->fdtable[fd1] = NULL;
        return ret;
    }
    return 0;
}

int64 sys_close(int fd) {
    struct proc *p = curr_proc();
    struct file *f = fdget(fd);
    if (f == NULL) {
        return -EBADF;
    }
    fput(f);
    p->fdtable[fd] = NULL;
    return 0;
}

int64 sys_gettimeofday(uint64 __user utv) {
    struct timeval tv;

    acquire(&tickslock);
    uint64 now = ticks;
    release(&tickslock);

    tv.sec  = now / TICKS_PER_SEC;
    tv.usec = (now % TICKS_PER_SEC) * (1000000 / TICKS_PER_SEC);

    struct proc *p = curr_proc();
    acquire(&p->mm->lock);
    int ret = copy_to_user(p->mm, utv, (char *)&tv, sizeof(tv));
    release(&p->mm->lock);
    return ret;
}

int64 sys_open(uint64 __user upath, uint64 flags) {
    int ret;
    char *path = NULL;
    struct file *f = NULL;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;

    ret = vfs_open(&f, path, flags);
    kfree(&kstrbuf, path);
    if (ret < 0)
        return ret;

    int fd = fdalloc(f);
    if (fd < 0) {
        vfs_close(f);
        return -EBADF;
    }
    return fd;
}

int64 sys_lseek(int fd, uint64 offset, uint64 whence) {
    struct file *f = fdget(fd);
    if (f == NULL)
        return -EBADF;
    return vfs_lseek(f, offset, whence);
}

int64 sys_mkdir(uint64 __user upath) {
    int ret;
    char *path = NULL;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;
    ret = vfs_mkdir(path);
    kfree(&kstrbuf, path);
    return ret;
}

int64 sys_rmdir(uint64 __user upath) {
    int ret;
    char *path = NULL;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;
    ret = vfs_rmdir(path);
    kfree(&kstrbuf, path);
    return ret;
}

int64 sys_unlink(uint64 __user upath) {
    int ret;
    char *path = NULL;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;
    ret = vfs_unlink(path);
    kfree(&kstrbuf, path);
    return ret;
}

int64 sys_getdents(int fd, uint64 __user va, uint64 len) {
    struct file *f = fdget(fd);
    if (f == NULL)
        return -EBADF;
    return vfs_getdents(f, (char *)va, len);
}

int64 sys_stat(uint64 __user upath, uint64 __user ust) {
    int ret;
    char *path = NULL;
    struct stat st;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;

    struct inode *inode = dlookup(path);
    kfree(&kstrbuf, path);
    if (inode == NULL)
        return -ENOENT;

    fill_stat_locked(inode, &st);
    iunlockput(inode);
    return copy_stat_to_user(ust, &st);
}

int64 sys_fstat(int fd, uint64 __user ust) {
    struct file *f = fdget(fd);
    if (f == NULL)
        return -EBADF;

    struct inode *inode = file_inode(f);
    if (inode == NULL)
        return -EINVAL;

    struct stat st;
    ilock(inode);
    fill_stat_locked(inode, &st);
    iunlock(inode);
    return copy_stat_to_user(ust, &st);
}

int64 sys_mkfifo(uint64 __user upath) {
    int ret;
    char *path = NULL;

    if ((ret = fetch_path(upath, &path)) < 0)
        return ret;
    ret = vfs_mkfifo(path);
    kfree(&kstrbuf, path);
    return ret;
}

void syscall() {
    struct trapframe *trapframe = curr_proc()->trapframe;
    int id                      = trapframe->a7;
    uint64 ret;
    uint64 args[6] = {trapframe->a0, trapframe->a1, trapframe->a2, trapframe->a3, trapframe->a4, trapframe->a5};
    tracef("syscall %d args = [%x, %x, %x, %x, %x, %x]", id, args[0], args[1], args[2], args[3], args[4], args[5]);
    switch (id) {
        case SYS_fork:
            ret = sys_fork();
            break;
        case SYS_exec:
            ret = sys_exec(args[0], args[1]);
            break;
        case SYS_exit:
            sys_exit(args[0]);
            panic_never_reach();
        case SYS_wait:
            ret = sys_wait(args[0], args[1]);
            break;
        case SYS_getpid:
            ret = sys_getpid();
            break;
        case SYS_getppid:
            ret = sys_getppid();
            break;
        case SYS_kill:
            ret = sys_kill(args[0]);
            break;
        case SYS_sleep:
            ret = sys_sleep(args[0]);
            break;
        case SYS_yield:
            ret = sys_yield();
            break;
        case SYS_sbrk:
            ret = sys_sbrk(args[0]);
            break;
        case SYS_mmap:
            ret = sys_mmap();
            break;
        case SYS_read:
            ret = sys_read(args[0], args[1], args[2]);
            break;
        case SYS_write:
            ret = sys_write(args[0], args[1], args[2]);
            break;
        case SYS_pipe:
            ret = sys_pipe((int *)args[0]);
            break;
        case SYS_close:
            ret = sys_close(args[0]);
            break;
        case SYS_gettimeofday:
            ret = sys_gettimeofday(args[0]);
            break;
        case SYS_open:
            ret = sys_open(args[0], args[1]);
            break;
        case SYS_lseek:
            ret = sys_lseek(args[0], args[1], args[2]);
            break;
        case SYS_mkdir:
            ret = sys_mkdir(args[0]);
            break;
        case SYS_rmdir:
            ret = sys_rmdir(args[0]);
            break;
        case SYS_unlink:
            ret = sys_unlink(args[0]);
            break;
        case SYS_getdents:
            ret = sys_getdents(args[0], args[1], args[2]);
            break;
        case SYS_stat:
            ret = sys_stat(args[0], args[1]);
            break;
        case SYS_fstat:
            ret = sys_fstat(args[0], args[1]);
            break;
        case SYS_mkfifo:
            ret = sys_mkfifo(args[0]);
            break;
        case SYS_ktest:
            ret = ktest_syscall(args);
            break;
        default:
            ret = -1;
            errorf("unknown syscall %d", id);
    }
    trapframe->a0 = ret;
    tracef("syscall ret %d", ret);
}
