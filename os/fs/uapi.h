#ifndef FS_UAPI_H
#define FS_UAPI_H

#include "../types.h"

#define O_RDONLY 0x001
#define O_WRONLY 0x002
#define O_RDWR   0x003
#define O_CREAT  0x200
#define O_TRUNC  0x400

#define SEEK_SET 1
#define SEEK_CUR 2
#define SEEK_END 3

#define ST_MODE_DEVICE 0x100
#define ST_MODE_REG    0x200
#define ST_MODE_DIR    0x400
#define ST_MODE_FIFO   0x800

#define DIRENT_NAME_MAX 28

struct dirent {
    uint32 ino;
    char name[DIRENT_NAME_MAX];
};

struct stat {
    uint32 ino;
    uint32 mode;
    uint32 nlinks;
    uint32 _pad;
    uint64 size;
};

struct timeval {
    uint64 sec;
    uint64 usec;
};

#endif  // FS_UAPI_H
