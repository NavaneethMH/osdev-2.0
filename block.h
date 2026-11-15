#ifndef TINYOS_BLOCK_H
#define TINYOS_BLOCK_H

#include "disk.h"

struct block_device {
    const char *name;
    unsigned int sector_size;
    int (*initialize)(void);
    unsigned int (*sector_count)(void);
    int (*read_sectors)(unsigned int lba, unsigned int count,
                        unsigned char *buffer);
    int (*write_sectors)(unsigned int lba, unsigned int count,
                         const unsigned char *buffer);
    const char *(*error_string)(int code);
    int (*is_ready)(void);
};

/* The filesystem uses this stable block-device interface instead of knowing
   which controller implements the primary disk. */
const struct block_device *block_primary_device(void);
int block_initialize(void);
unsigned int block_sector_count(void);
int block_read_sectors(unsigned int lba, unsigned int count,
                       unsigned char *buffer);
int block_write_sectors(unsigned int lba, unsigned int count,
                        const unsigned char *buffer);
const char *block_error_string(int code);
int block_is_ready(void);

#endif

