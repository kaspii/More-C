#ifndef OSPRD_H
#define OSPRD_H
#include <linux/types.h>

// ioctl constants
#define OSPRDIOCACQUIRE		42
#define OSPRDIOCTRYACQUIRE	43
#define OSPRDIOCRELEASE		44
#define OSPRDIONOTIFY		45

typedef struct reqParams
{
	sector_t sector;
	unsigned nSectors;
} reqParams_t;

#endif
