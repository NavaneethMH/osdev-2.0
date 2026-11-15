#ifndef TINYOS_FRAMEBUFFER_H
#define TINYOS_FRAMEBUFFER_H

enum framebuffer_status {
    FRAMEBUFFER_UNAVAILABLE = 0,
    FRAMEBUFFER_VALID = 1,
    FRAMEBUFFER_BAD_ADDRESS = 2,
    FRAMEBUFFER_BAD_DIMENSIONS = 3,
    FRAMEBUFFER_BAD_PITCH = 4,
    FRAMEBUFFER_BAD_FORMAT = 5,
    FRAMEBUFFER_BAD_BOUNDS = 6
};

struct framebuffer_info {
    unsigned int address_low;
    unsigned int address_high;
    unsigned int pitch;
    unsigned int width;
    unsigned int height;
    unsigned char bits_per_pixel;
    unsigned char memory_model;
    unsigned char red_position;
    unsigned char red_size;
    unsigned char green_position;
    unsigned char green_size;
    unsigned char blue_position;
    unsigned char blue_size;
    unsigned int available;
    enum framebuffer_status status;
    unsigned int byte_length_low;
    unsigned int byte_length_high;
};

enum framebuffer_status framebuffer_validate(struct framebuffer_info *info);
const char *framebuffer_status_name(enum framebuffer_status status);

#endif

