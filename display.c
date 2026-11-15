#include "display.h"

#define DISPLAY_WIDTH 80U
#define DISPLAY_HEIGHT 25U
#define DISPLAY_CELLS (DISPLAY_WIDTH * DISPLAY_HEIGHT)
#define DISPLAY_ATTRIBUTE 0x0F00U

static volatile unsigned short *const video =
    (volatile unsigned short *)0xB8000U;
static unsigned int cursor = 0U;

static void scroll_if_needed(void)
{
    if (cursor < DISPLAY_CELLS) {
        return;
    }

    for (unsigned int i = 0U; i < DISPLAY_WIDTH * (DISPLAY_HEIGHT - 1U); ++i) {
        video[i] = video[i + DISPLAY_WIDTH];
    }
    for (unsigned int i = DISPLAY_WIDTH * (DISPLAY_HEIGHT - 1U);
         i < DISPLAY_CELLS; ++i) {
        video[i] = (unsigned short)(DISPLAY_ATTRIBUTE | ' ');
    }
    cursor = DISPLAY_WIDTH * (DISPLAY_HEIGHT - 1U);
}

void display_initialize(void)
{
    display_clear();
}

void display_clear(void)
{
    for (unsigned int i = 0U; i < DISPLAY_CELLS; ++i) {
        video[i] = (unsigned short)(DISPLAY_ATTRIBUTE | ' ');
    }
    cursor = 0U;
}

void display_put_char(char character)
{
    if (character == '\n') {
        cursor = (cursor / DISPLAY_WIDTH + 1U) * DISPLAY_WIDTH;
    } else if (character == '\b') {
        if (cursor > 0U) {
            --cursor;
            video[cursor] = (unsigned short)(DISPLAY_ATTRIBUTE | ' ');
        }
    } else {
        video[cursor] = (unsigned short)(DISPLAY_ATTRIBUTE |
                                         (unsigned char)character);
        ++cursor;
    }
    scroll_if_needed();
}

