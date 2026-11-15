#include "framebuffer.h"

#define FRAMEBUFFER_RGB_MODEL 1U
#define FRAMEBUFFER_MAX_DIMENSION 8192U
#define FRAMEBUFFER_MAX_BYTES (512ULL * 1024ULL * 1024ULL)

static unsigned long long channel_mask(unsigned char position,
                                       unsigned char size)
{
    unsigned long long bits = (1ULL << size) - 1ULL;
    return bits << position;
}

enum framebuffer_status framebuffer_validate(struct framebuffer_info *info)
{
    if (info == 0 || info->available == 0U) {
        return FRAMEBUFFER_UNAVAILABLE;
    }

    info->byte_length_low = 0U;
    info->byte_length_high = 0U;

    if (info->address_high != 0U || info->address_low == 0U) {
        info->status = FRAMEBUFFER_BAD_ADDRESS;
        return info->status;
    }

    if (info->width == 0U || info->height == 0U ||
        info->width > FRAMEBUFFER_MAX_DIMENSION ||
        info->height > FRAMEBUFFER_MAX_DIMENSION) {
        info->status = FRAMEBUFFER_BAD_DIMENSIONS;
        return info->status;
    }

    if (info->memory_model != FRAMEBUFFER_RGB_MODEL ||
        (info->bits_per_pixel != 15U && info->bits_per_pixel != 16U &&
         info->bits_per_pixel != 24U && info->bits_per_pixel != 32U) ||
        info->red_size == 0U || info->green_size == 0U ||
        info->blue_size == 0U ||
        (unsigned int)info->red_position + info->red_size >
            info->bits_per_pixel ||
        (unsigned int)info->green_position + info->green_size >
            info->bits_per_pixel ||
        (unsigned int)info->blue_position + info->blue_size >
            info->bits_per_pixel) {
        info->status = FRAMEBUFFER_BAD_FORMAT;
        return info->status;
    }

    unsigned long long red = channel_mask(info->red_position, info->red_size);
    unsigned long long green = channel_mask(info->green_position,
                                            info->green_size);
    unsigned long long blue = channel_mask(info->blue_position,
                                           info->blue_size);
    if ((red & green) != 0ULL || (red & blue) != 0ULL ||
        (green & blue) != 0ULL) {
        info->status = FRAMEBUFFER_BAD_FORMAT;
        return info->status;
    }

    unsigned int bytes_per_pixel =
        ((unsigned int)info->bits_per_pixel + 7U) / 8U;
    unsigned long long minimum_pitch =
        (unsigned long long)info->width * bytes_per_pixel;
    if ((unsigned long long)info->pitch < minimum_pitch) {
        info->status = FRAMEBUFFER_BAD_PITCH;
        return info->status;
    }

    unsigned long long length =
        (unsigned long long)info->pitch * info->height;
    unsigned long long end = (unsigned long long)info->address_low + length;
    if (length == 0ULL || length > FRAMEBUFFER_MAX_BYTES ||
        end > 0x100000000ULL) {
        info->status = FRAMEBUFFER_BAD_BOUNDS;
        return info->status;
    }

    info->byte_length_low = (unsigned int)length;
    info->byte_length_high = (unsigned int)(length >> 32);
    info->status = FRAMEBUFFER_VALID;
    return info->status;
}

const char *framebuffer_status_name(enum framebuffer_status status)
{
    switch (status) {
        case FRAMEBUFFER_VALID: return "valid RGB framebuffer";
        case FRAMEBUFFER_BAD_ADDRESS: return "invalid or unsupported address";
        case FRAMEBUFFER_BAD_DIMENSIONS: return "invalid dimensions";
        case FRAMEBUFFER_BAD_PITCH: return "pitch is too small for a row";
        case FRAMEBUFFER_BAD_FORMAT: return "unsupported pixel format";
        case FRAMEBUFFER_BAD_BOUNDS: return "framebuffer range is out of bounds";
        case FRAMEBUFFER_UNAVAILABLE:
        default: return "bootloader framebuffer information unavailable";
    }
}

