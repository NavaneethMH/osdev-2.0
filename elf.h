#ifndef ELF_H
#define ELF_H

#define ELF32_MAX_LOAD_SEGMENTS 16U

struct elf32_load_segment {
    unsigned int offset;
    unsigned int virtual_address;
    unsigned int file_size;
    unsigned int memory_size;
    unsigned int flags;
    unsigned int alignment;
};

struct elf32_image_info {
    unsigned int entry;
    unsigned int load_segment_count;
    unsigned int lowest_address;
    unsigned int highest_address;
    struct elf32_load_segment segments[ELF32_MAX_LOAD_SEGMENTS];
};

enum elf32_result {
    ELF32_VALID = 0,
    ELF32_BAD_ARGUMENT,
    ELF32_TRUNCATED_HEADER,
    ELF32_BAD_MAGIC,
    ELF32_UNSUPPORTED_FORMAT,
    ELF32_BAD_HEADER,
    ELF32_BAD_PROGRAM_TABLE,
    ELF32_BAD_LOAD_SEGMENT,
    ELF32_NO_LOAD_SEGMENTS,
    ELF32_ENTRY_NOT_EXECUTABLE,
    ELF32_OVERLAPPING_SEGMENTS
};

enum elf32_result elf32_validate(const unsigned char *image,
                                 unsigned int image_size,
                                 struct elf32_image_info *info);
int elf32_validation_demo(void);

#endif

