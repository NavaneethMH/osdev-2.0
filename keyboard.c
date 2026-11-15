#include "keyboard.h"

#define KEYBOARD_BUFFER_SIZE 256U

static volatile char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile unsigned int keyboard_head = 0U;
static volatile unsigned int keyboard_tail = 0U;
static volatile unsigned int keyboard_count = 0U;
static volatile int left_shift_down = 0;
static volatile int right_shift_down = 0;
static volatile int caps_lock = 0;

static unsigned char inb(unsigned short port)
{
    unsigned char value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static char key_from_scancode(unsigned char code, int shifted, int caps)
{
    char character;
    switch (code) {
        case 0x02: character = '1'; break; case 0x03: character = '2'; break;
        case 0x04: character = '3'; break; case 0x05: character = '4'; break;
        case 0x06: character = '5'; break; case 0x07: character = '6'; break;
        case 0x08: character = '7'; break; case 0x09: character = '8'; break;
        case 0x0A: character = '9'; break; case 0x0B: character = '0'; break;
        case 0x0C: character = '-'; break; case 0x0D: character = '='; break;
        case 0x0E: return '\b';
        case 0x10: character = 'q'; break; case 0x11: character = 'w'; break;
        case 0x12: character = 'e'; break; case 0x13: character = 'r'; break;
        case 0x14: character = 't'; break; case 0x15: character = 'y'; break;
        case 0x16: character = 'u'; break; case 0x17: character = 'i'; break;
        case 0x18: character = 'o'; break; case 0x19: character = 'p'; break;
        case 0x1A: character = '['; break; case 0x1B: character = ']'; break;
        case 0x1C: return '\n';
        case 0x1E: character = 'a'; break; case 0x1F: character = 's'; break;
        case 0x20: character = 'd'; break; case 0x21: character = 'f'; break;
        case 0x22: character = 'g'; break; case 0x23: character = 'h'; break;
        case 0x24: character = 'j'; break; case 0x25: character = 'k'; break;
        case 0x26: character = 'l'; break; case 0x27: character = ';'; break;
        case 0x28: character = '\''; break; case 0x29: character = '`'; break;
        case 0x2B: character = '\\'; break; case 0x2C: character = 'z'; break;
        case 0x2D: character = 'x'; break; case 0x2E: character = 'c'; break;
        case 0x2F: character = 'v'; break; case 0x30: character = 'b'; break;
        case 0x31: character = 'n'; break; case 0x32: character = 'm'; break;
        case 0x33: character = ','; break; case 0x34: character = '.'; break;
        case 0x35: character = '/'; break; case 0x39: character = ' '; break;
        default: return '\0';
    }

    if (character >= 'a' && character <= 'z') {
        if (shifted != caps) {
            return (char)(character - 'a' + 'A');
        }
        return character;
    }
    if (!shifted) {
        return character;
    }

    switch (code) {
        case 0x02: return '!'; case 0x03: return '@'; case 0x04: return '#';
        case 0x05: return '$'; case 0x06: return '%'; case 0x07: return '^';
        case 0x08: return '&'; case 0x09: return '*'; case 0x0A: return '(';
        case 0x0B: return ')'; case 0x0C: return '_'; case 0x0D: return '+';
        case 0x1A: return '{'; case 0x1B: return '}'; case 0x27: return ':';
        case 0x28: return '"'; case 0x29: return '~'; case 0x2B: return '|';
        case 0x33: return '<'; case 0x34: return '>'; case 0x35: return '?';
        default: return character;
    }
}

void keyboard_initialize(void)
{
    keyboard_head = 0U;
    keyboard_tail = 0U;
    keyboard_count = 0U;
    left_shift_down = 0;
    right_shift_down = 0;
    caps_lock = 0;
}

void keyboard_handle_interrupt(void)
{
    unsigned char code = inb(0x60U);
    if (code == 0x2AU) { left_shift_down = 1; return; }
    if (code == 0x36U) { right_shift_down = 1; return; }
    if (code == 0xAAU) { left_shift_down = 0; return; }
    if (code == 0xB6U) { right_shift_down = 0; return; }
    if (code == 0x3AU) { caps_lock = !caps_lock; return; }
    if ((code & 0x80U) != 0U) {
        return;
    }

    char character = key_from_scancode(
        code, left_shift_down || right_shift_down, caps_lock);
    if (character == '\0' || keyboard_count >= KEYBOARD_BUFFER_SIZE) {
        return;
    }
    keyboard_buffer[keyboard_head] = character;
    keyboard_head = (keyboard_head + 1U) % KEYBOARD_BUFFER_SIZE;
    ++keyboard_count;
}

int keyboard_has_input(void)
{
    return keyboard_count != 0U;
}

char keyboard_read_char(void)
{
    if (keyboard_count == 0U) {
        return '\0';
    }
    char character = keyboard_buffer[keyboard_tail];
    keyboard_tail = (keyboard_tail + 1U) % KEYBOARD_BUFFER_SIZE;
    --keyboard_count;
    return character;
}

