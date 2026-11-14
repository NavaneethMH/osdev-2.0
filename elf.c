#include "elf.h"

#define ELF32_HEADER_SIZE 52U
#define ELF32_PROGRAM_HEADER_SIZE 32U
#define ELF32_MAX_PROGRAM_HEADERS 32U
#define ELF32_MAX_LOAD_SEGMENTS 16U
#define ELF32_USER_MIN 0x00400000U
#define ELF32_USER_MAX 0xC0000000U

#define ELF32_ET_EXEC 2U
#define ELF32_EM_386 3U
#define ELF32_EV_CURRENT 1U
#define ELF32_PT_LOAD 1U
#define ELF32_PF_X 1U
#define ELF32_PF_W 2U
#define ELF32_PF_R 4U

struct elf32_load_range {
    unsigned int start;
    unsigned int end;
    unsigned int flags;
};

static unsigned int read_u16(const unsigned char *bytes)
{
    return (unsigned int)bytes[0] | ((unsigned int)bytes[1] << 8);
}

static unsigned int read_u32(const unsigned char *bytes)
{
    return (unsigned int)bytes[0] |
           ((unsigned int)bytes[1] << 8) |
           ((unsigned int)bytes[2] << 16) |
           ((unsigned int)bytes[3] << 24);
}

static void write_u16(unsigned char *bytes, unsigned int value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
}

static void write_u32(unsigned char *bytes, unsigned int value)
{
    bytes[0] = (unsigned char)value;
    bytes[1] = (unsigned char)(value >> 8);
    bytes[2] = (unsigned char)(value >> 16);
    bytes[3] = (unsigned char)(value >> 24);
}

enum elf32_result elf32_validate(const unsigned char *image,
                                 unsigned int image_size,
                                 struct elf32_image_info *info)
{
    struct elf32_load_range ranges[ELF32_MAX_LOAD_SEGMENTS];
    unsigned int entry;
    unsigned int phoff;
    unsigned int phnum;
    unsigned int load_count = 0U;
    unsigned int lowest = 0xFFFFFFFFU;
    unsigned int highest = 0U;
    int entry_is_executable = 0;

    if (image == 0 || info == 0) {
        return ELF32_BAD_ARGUMENT;
    }
    if (image_size < ELF32_HEADER_SIZE) {
        return ELF32_TRUNCATED_HEADER;
    }
    if (image[0] != 0x7FU || image[1] != 'E' ||
        image[2] != 'L' || image[3] != 'F') {
        return ELF32_BAD_MAGIC;
    }
    if (image[4] != 1U || image[5] != 1U || image[6] != ELF32_EV_CURRENT ||
        image[7] != 0U) {
        return ELF32_UNSUPPORTED_FORMAT;
    }

    if (read_u16(&image[16]) != ELF32_ET_EXEC ||
        read_u16(&image[18]) != ELF32_EM_386 ||
        read_u32(&image[20]) != ELF32_EV_CURRENT ||
        read_u16(&image[40]) != ELF32_HEADER_SIZE ||
        read_u16(&image[42]) != ELF32_PROGRAM_HEADER_SIZE ||
        read_u16(&image[44]) == 0U) {
        return ELF32_BAD_HEADER;
    }

    entry = read_u32(&image[24]);
    phoff = read_u32(&image[28]);
    phnum = read_u16(&image[44]);
    if (phnum > ELF32_MAX_PROGRAM_HEADERS || phoff < ELF32_HEADER_SIZE ||
        phoff > image_size ||
        phnum > (image_size - phoff) / ELF32_PROGRAM_HEADER_SIZE) {
        return ELF32_BAD_PROGRAM_TABLE;
    }

    for (unsigned int i = 0U; i < phnum; ++i) {
        const unsigned char *ph = &image[phoff + i * ELF32_PROGRAM_HEADER_SIZE];
        unsigned int type = read_u32(&ph[0]);

        if (type == ELF32_PT_LOAD) {
            unsigned int offset = read_u32(&ph[4]);
            unsigned int vaddr = read_u32(&ph[8]);
            unsigned int filesz = read_u32(&ph[16]);
            unsigned int memsz = read_u32(&ph[20]);
            unsigned int flags = read_u32(&ph[24]);
            unsigned int align = read_u32(&ph[28]);
            unsigned int end;

            if (load_count >= ELF32_MAX_LOAD_SEGMENTS || memsz == 0U ||
                filesz > memsz || offset > image_size ||
                filesz > image_size - offset || vaddr < ELF32_USER_MIN ||
                memsz > 0xFFFFFFFFU - vaddr) {
                return ELF32_BAD_LOAD_SEGMENT;
            }
            end = vaddr + memsz;
            if (end > ELF32_USER_MAX || (flags & ~(ELF32_PF_R | ELF32_PF_W | ELF32_PF_X)) != 0U ||
                ((flags & (ELF32_PF_W | ELF32_PF_X)) ==
                 (ELF32_PF_W | ELF32_PF_X))) {
                return ELF32_BAD_LOAD_SEGMENT;
            }
            if (align > 1U &&
                ((align & (align - 1U)) != 0U ||
                 (offset & (align - 1U)) != (vaddr & (align - 1U)))) {
                return ELF32_BAD_LOAD_SEGMENT;
            }

            ranges[load_count].start = vaddr;
            ranges[load_count].end = end;
            ranges[load_count].flags = flags;
            for (unsigned int j = 0U; j < load_count; ++j) {
                if (vaddr < ranges[j].end && ranges[j].start < end) {
                    return ELF32_OVERLAPPING_SEGMENTS;
                }
            }
            if (vaddr < lowest) {
                lowest = vaddr;
            }
            if (end > highest) {
                highest = end;
            }
            if ((flags & ELF32_PF_X) != 0U && entry >= vaddr &&
                entry < vaddr + filesz) {
                entry_is_executable = 1;
            }
            ++load_count;
        }
    }

    if (load_count == 0U) {
        return ELF32_NO_LOAD_SEGMENTS;
    }
    if (!entry_is_executable) {
        return ELF32_ENTRY_NOT_EXECUTABLE;
    }

    info->entry = entry;
    info->load_segment_count = load_count;
    info->lowest_address = lowest;
    info->highest_address = highest;
    return ELF32_VALID;
}

static void make_demo_image(unsigned char *image)
{
    for (unsigned int i = 0U; i < 88U; ++i) {
        image[i] = 0U;
    }
    image[0] = 0x7FU;
    image[1] = 'E';
    image[2] = 'L';
    image[3] = 'F';
    image[4] = 1U;
    image[5] = 1U;
    image[6] = 1U;
    write_u16(&image[16], ELF32_ET_EXEC);
    write_u16(&image[18], ELF32_EM_386);
    write_u32(&image[20], ELF32_EV_CURRENT);
    write_u32(&image[24], ELF32_USER_MIN);
    write_u32(&image[28], ELF32_HEADER_SIZE);
    write_u16(&image[40], ELF32_HEADER_SIZE);
    write_u16(&image[42], ELF32_PROGRAM_HEADER_SIZE);
    write_u16(&image[44], 1U);

    write_u32(&image[52], ELF32_PT_LOAD);
    write_u32(&image[56], 84U);
    write_u32(&image[60], ELF32_USER_MIN);
    write_u32(&image[68], 4U);
    write_u32(&image[72], 8U);
    write_u32(&image[76], ELF32_PF_R | ELF32_PF_X);
    write_u32(&image[80], 1U);
    image[84] = 0x90U;
    image[85] = 0x90U;
    image[86] = 0x90U;
    image[87] = 0xC3U;
}

int elf32_validation_demo(void)
{
    unsigned char image[88];
    struct elf32_image_info info;
    int rejected = 0;

    make_demo_image(image);
    if (elf32_validate(image, sizeof(image), &info) != ELF32_VALID ||
        info.entry != ELF32_USER_MIN || info.load_segment_count != 1U ||
        info.lowest_address != ELF32_USER_MIN ||
        info.highest_address != ELF32_USER_MIN + 8U) {
        return 0;
    }

    image[0] = 0U;
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_BAD_MAGIC;
    make_demo_image(image);
    image[4] = 2U;
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_UNSUPPORTED_FORMAT;
    make_demo_image(image);
    write_u32(&image[28], 0xFFFFFFF0U);
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_BAD_PROGRAM_TABLE;
    make_demo_image(image);
    write_u32(&image[68], 9U);
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_BAD_LOAD_SEGMENT;
    make_demo_image(image);
    write_u32(&image[60], 0xBFFFFFFCU);
    write_u32(&image[72], 8U);
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_BAD_LOAD_SEGMENT;
    make_demo_image(image);
    write_u32(&image[24], ELF32_USER_MIN + 16U);
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_ENTRY_NOT_EXECUTABLE;
    make_demo_image(image);
    write_u32(&image[76], ELF32_PF_R | ELF32_PF_W | ELF32_PF_X);
    rejected += elf32_validate(image, sizeof(image), &info) == ELF32_BAD_LOAD_SEGMENT;

    return rejected == 7;
}

