#include "block.h"

static const struct block_device primary_device = {
    "primary IDE",
    DISK_SECTOR_SIZE,
    disk_initialize,
    disk_sector_count,
    disk_read_sectors,
    disk_write_sectors,
    disk_error_string,
    disk_is_ready
};

const struct block_device *block_primary_device(void)
{
    return &primary_device;
}

int block_initialize(void)
{
    return block_primary_device()->initialize();
}

unsigned int block_sector_count(void)
{
    return block_primary_device()->sector_count();
}

int block_read_sectors(unsigned int lba, unsigned int count,
                       unsigned char *buffer)
{
    return block_primary_device()->read_sectors(lba, count, buffer);
}

int block_write_sectors(unsigned int lba, unsigned int count,
                        const unsigned char *buffer)
{
    return block_primary_device()->write_sectors(lba, count, buffer);
}

const char *block_error_string(int code)
{
    return block_primary_device()->error_string(code);
}

int block_is_ready(void)
{
    return block_primary_device()->is_ready();
}

