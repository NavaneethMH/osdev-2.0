#include "disk.h"

#define ATA_DATA       0x1F0U
#define ATA_ERROR      0x1F1U
#define ATA_SECTOR_CNT 0x1F2U
#define ATA_LBA_LOW    0x1F3U
#define ATA_LBA_MID    0x1F4U
#define ATA_LBA_HIGH   0x1F5U
#define ATA_DEVICE     0x1F6U
#define ATA_STATUS     0x1F7U
#define ATA_COMMAND    0x1F7U
#define ATA_ALT_STATUS 0x3F6U

#define ATA_STATUS_ERR  0x01U
#define ATA_STATUS_DRQ  0x08U
#define ATA_STATUS_DF   0x20U
#define ATA_STATUS_DRDY 0x40U
#define ATA_STATUS_BSY  0x80U

#define ATA_CMD_IDENTIFY 0xECU
#define ATA_CMD_READ     0x20U
#define ATA_CMD_WRITE    0x30U
#define ATA_CMD_FLUSH    0xE7U

#define ATA_POLL_LIMIT 1000000U
#define ATA_LBA28_LIMIT 0x10000000U

enum {
    ATA_WAIT_NOT_BUSY = 0,
    ATA_WAIT_DATA = 1,
    ATA_WAIT_READY = 2
};

enum {
    DISK_ERR_NO_DEVICE = -1,
    DISK_ERR_TIMEOUT = -2,
    DISK_ERR_DEVICE = -3,
    DISK_ERR_NO_LBA = -4,
    DISK_ERR_RANGE = -5,
    DISK_ERR_ARGUMENT = -6
};

static unsigned int identified_sectors;
static int initialization_attempted;
static int initialization_result = DISK_ERR_NO_DEVICE;

static void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static unsigned short inw(unsigned short port)
{
    unsigned short value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(unsigned short port, unsigned short value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* Four alternate-status reads provide the small delay required after select. */
static void ata_delay_400ns(void)
{
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
    (void)inb(ATA_ALT_STATUS);
}

/* Pre-command waits only require BSY clear. Requiring DRDY/DRQ state there
   can deadlock on emulated IDE devices after a read; completion waits use the
   stricter ready-without-data state, while data phases wait for DRQ. */
static int ata_wait(int mode)
{
    for (unsigned int i = 0; i < ATA_POLL_LIMIT; ++i) {
        unsigned char status = inb(ATA_STATUS);
        if (status == 0U || status == 0xFFU) {
            return DISK_ERR_NO_DEVICE;
        }
        if ((status & ATA_STATUS_BSY) != 0U) {
            continue;
        }
        if ((status & (ATA_STATUS_ERR | ATA_STATUS_DF)) != 0U) {
            (void)inb(ATA_ERROR);
            return DISK_ERR_DEVICE;
        }
        if (mode == ATA_WAIT_DATA && (status & ATA_STATUS_DRQ) == 0U) {
            continue;
        }
        if (mode == ATA_WAIT_READY &&
            ((status & ATA_STATUS_DRDY) == 0U ||
             (status & ATA_STATUS_DRQ) != 0U)) {
            continue;
        }
        return 0;
    }
    return DISK_ERR_TIMEOUT;
}

static void ata_select_master(void)
{
    outb(ATA_DEVICE, 0xA0U);
    ata_delay_400ns();
}

int disk_initialize(void)
{
    if (initialization_attempted) {
        return initialization_result;
    }
    initialization_attempted = 1;

    unsigned short identify[256];
    ata_select_master();
    outb(ATA_SECTOR_CNT, 0U);
    outb(ATA_LBA_LOW, 0U);
    outb(ATA_LBA_MID, 0U);
    outb(ATA_LBA_HIGH, 0U);
    outb(ATA_COMMAND, ATA_CMD_IDENTIFY);

    unsigned char status = inb(ATA_STATUS);
    if (status == 0U || status == 0xFFU) {
        initialization_result = DISK_ERR_NO_DEVICE;
        return initialization_result;
    }

    int result = ata_wait(ATA_WAIT_DATA);
    if (result != 0) {
        initialization_result = result;
        return initialization_result;
    }

    /* ATAPI devices report a non-zero signature in these registers. */
    if (inb(ATA_LBA_MID) != 0U || inb(ATA_LBA_HIGH) != 0U) {
        initialization_result = DISK_ERR_NO_DEVICE;
        return initialization_result;
    }

    for (unsigned int i = 0; i < 256U; ++i) {
        identify[i] = inw(ATA_DATA);
    }

    if ((identify[49] & (1U << 9)) == 0U) {
        initialization_result = DISK_ERR_NO_LBA;
        return initialization_result;
    }

    identified_sectors = (unsigned int)identify[60] |
                         ((unsigned int)identify[61] << 16);
    if (identified_sectors == 0U) {
        initialization_result = DISK_ERR_NO_DEVICE;
        return initialization_result;
    }
    if (identified_sectors > ATA_LBA28_LIMIT) {
        identified_sectors = ATA_LBA28_LIMIT;
    }

    initialization_result = 0;
    return 0;
}

unsigned int disk_sector_count(void)
{
    if (disk_initialize() != 0) {
        return 0U;
    }
    return identified_sectors;
}

int disk_is_ready(void)
{
    return disk_initialize() == 0;
}

static int disk_range_valid(unsigned int lba, unsigned int count,
                            const unsigned char *buffer)
{
    if (buffer == 0 || count == 0U || count > 255U) {
        return DISK_ERR_ARGUMENT;
    }
    if (disk_initialize() != 0) {
        return initialization_result;
    }
    if (lba >= identified_sectors || count > identified_sectors - lba ||
        lba >= ATA_LBA28_LIMIT || count > ATA_LBA28_LIMIT - lba) {
        return DISK_ERR_RANGE;
    }
    return 0;
}

static void ata_program_lba(unsigned int lba)
{
    outb(ATA_DEVICE, (unsigned char)(0xE0U | ((lba >> 24) & 0x0FU)));
    ata_delay_400ns();
    outb(ATA_SECTOR_CNT, 1U);
    outb(ATA_LBA_LOW, (unsigned char)(lba & 0xFFU));
    outb(ATA_LBA_MID, (unsigned char)((lba >> 8) & 0xFFU));
    outb(ATA_LBA_HIGH, (unsigned char)((lba >> 16) & 0xFFU));
}

int disk_read_sectors(unsigned int lba, unsigned int count,
                      unsigned char *buffer)
{
    int result = disk_range_valid(lba, count, buffer);
    if (result != 0) {
        return result;
    }

    for (unsigned int sector = 0; sector < count; ++sector) {
        result = ata_wait(ATA_WAIT_NOT_BUSY);
        if (result != 0) {
            return result;
        }
        ata_program_lba(lba + sector);
        outb(ATA_COMMAND, ATA_CMD_READ);
        result = ata_wait(ATA_WAIT_DATA);
        if (result != 0) {
            return result;
        }
        for (unsigned int word = 0; word < DISK_SECTOR_SIZE / 2U; ++word) {
            unsigned short value = inw(ATA_DATA);
            buffer[sector * DISK_SECTOR_SIZE + word * 2U] =
                (unsigned char)(value & 0xFFU);
            buffer[sector * DISK_SECTOR_SIZE + word * 2U + 1U] =
                (unsigned char)(value >> 8);
        }
    }
    return (int)count;
}

int disk_write_sectors(unsigned int lba, unsigned int count,
                       const unsigned char *buffer)
{
    int result = disk_range_valid(lba, count, buffer);
    if (result != 0) {
        return result;
    }

    for (unsigned int sector = 0; sector < count; ++sector) {
        result = ata_wait(ATA_WAIT_NOT_BUSY);
        if (result != 0) {
            return result;
        }
        ata_program_lba(lba + sector);
        outb(ATA_COMMAND, ATA_CMD_WRITE);
        result = ata_wait(ATA_WAIT_DATA);
        if (result != 0) {
            return result;
        }
        for (unsigned int word = 0; word < DISK_SECTOR_SIZE / 2U; ++word) {
            unsigned int offset = sector * DISK_SECTOR_SIZE + word * 2U;
            unsigned short value = (unsigned short)buffer[offset] |
                ((unsigned short)buffer[offset + 1U] << 8);
            outw(ATA_DATA, value);
        }
        result = ata_wait(ATA_WAIT_READY);
        if (result != 0) {
            return result;
        }
    }

    outb(ATA_COMMAND, ATA_CMD_FLUSH);
    result = ata_wait(ATA_WAIT_READY);
    return result == 0 ? (int)count : result;
}

const char *disk_error_string(int code)
{
    switch (code) {
        case DISK_ERR_NO_DEVICE: return "no IDE disk detected";
        case DISK_ERR_TIMEOUT: return "disk operation timed out";
        case DISK_ERR_DEVICE: return "IDE disk reported an error";
        case DISK_ERR_NO_LBA: return "disk does not support LBA28";
        case DISK_ERR_RANGE: return "sector range is outside the disk";
        case DISK_ERR_ARGUMENT: return "invalid disk request";
        default: return "unknown disk error";
    }
}
