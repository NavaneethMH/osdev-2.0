#ifndef DISK_H
#define DISK_H

/*
 * TinyOS disk driver.
 *
 * Talks to the primary ATA/IDE master drive (I/O port 0x1F0) in LBA-28
 * mode. The disk image is a raw QEMU virtual disk created by the Makefile
 * (see disk/tinyos.img). Sector reads and writes are bounded, time out
 * cleanly, and never touch the host's real disks.
 *
 * NOTE: DISK_TOTAL_SECTORS and DISK_CAPACITY_MB must match the Makefile's
 * DISK_SIZE_MB (currently 16 MiB = 32768 sectors of 512 bytes).
 */

#define DISK_SECTOR_SIZE   512U    /* Bytes per sector. */
#define DISK_TOTAL_SECTORS 32768U  /* 16 MiB image: 32768 * 512 bytes. */
#define DISK_CAPACITY_MB   16U     /* Matches Makefile DISK_SIZE_MB. */
#define DISK_TEST_SECTOR   4096U   /* Reserved sector used by disktest. */

/* Identify the primary IDE master. Returns 0 when usable, negative on error. */
int disk_initialize(void);

/* Return the number of sectors identified, or 0 when unavailable. */
unsigned int disk_sector_count(void);

/* Read `count` sectors starting at LBA `lba` into `buffer`.
   Returns the number of sectors read on success, or a negative error
   code on failure. Max 255 sectors per call. */
int disk_read_sectors(unsigned int lba, unsigned int count, unsigned char *buffer);

/* Write `count` sectors from `buffer` to LBA `lba`, then flush the cache.
   Returns the number of sectors written on success, or a negative error
   code on failure. Max 255 sectors per call. */
int disk_write_sectors(unsigned int lba, unsigned int count, const unsigned char *buffer);

/* Human-readable explanation of a negative disk error code. */
const char *disk_error_string(int code);

/* Non-zero if the primary IDE master drive answers a ready poll. */
int disk_is_ready(void);

#endif /* DISK_H */

