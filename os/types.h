#ifndef TYPES_H
#define TYPES_H

typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;
typedef unsigned long ulong;
typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int uint32;
typedef unsigned long uint64;

typedef char int8;
typedef short int16;
typedef int int32;
typedef long int64;

typedef long long loff_t;

#define NULL  ((void *)0)
#define true  1
#define false 0

// errno

#define ENOMEM 1
#define EINVAL 2
#define ECHILD 3
#define ENOENT 4
#define EBADF  5
#define EEXIST 6
#define ENOSPC 7
#define ENOTEMPTY 8
#define EFBIG 9

#endif  // TYPES_H