#include "mouse.h"

#define PS2_DATA_PORT 0x60U
#define PS2_STATUS_PORT 0x64U
#define PS2_COMMAND_PORT 0x64U
#define PS2_OUTPUT_FULL 0x01U
#define PS2_INPUT_FULL 0x02U
#define PS2_AUX_DATA 0x20U
#define PS2_COMMAND_ENABLE_AUX 0xA8U
#define PS2_COMMAND_READ_CONFIG 0x20U
#define PS2_COMMAND_WRITE_CONFIG 0x60U
#define PS2_COMMAND_WRITE_AUX 0xD4U
#define PS2_MOUSE_ENABLE_STREAMING 0xF4U
#define PS2_MOUSE_ACK 0xFAU
#define PS2_WAIT_LIMIT 100000U
#define MOUSE_COORDINATE_LIMIT 32767

static volatile int mouse_ready = 0;
static volatile int relative_x = 0;
static volatile int relative_y = 0;
static volatile unsigned int packets = 0U;
static volatile unsigned int left_presses = 0U;
static volatile unsigned int right_presses = 0U;
static volatile unsigned int middle_presses = 0U;
static volatile unsigned char buttons = 0U;
static unsigned char packet[3];
static unsigned int packet_index = 0U;

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

static int wait_input_empty(void)
{
    for (unsigned int attempt = 0U; attempt < PS2_WAIT_LIMIT; ++attempt) {
        if ((inb(PS2_STATUS_PORT) & PS2_INPUT_FULL) == 0U) {
            return 1;
        }
    }
    return 0;
}

static int wait_output_full(void)
{
    for (unsigned int attempt = 0U; attempt < PS2_WAIT_LIMIT; ++attempt) {
        if ((inb(PS2_STATUS_PORT) & PS2_OUTPUT_FULL) != 0U) {
            return 1;
        }
    }
    return 0;
}

static int controller_read_config(unsigned char *config)
{
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND_PORT, PS2_COMMAND_READ_CONFIG);
    if (!wait_output_full()) {
        return 0;
    }
    *config = inb(PS2_DATA_PORT);
    return 1;
}

static int controller_write_config(unsigned char config)
{
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND_PORT, PS2_COMMAND_WRITE_CONFIG);
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_DATA_PORT, config);
    return 1;
}

static int send_auxiliary_command(unsigned char command)
{
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND_PORT, PS2_COMMAND_WRITE_AUX);
    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_DATA_PORT, command);
    if (!wait_output_full()) {
        return 0;
    }

    unsigned char status = inb(PS2_STATUS_PORT);
    unsigned char response = inb(PS2_DATA_PORT);
    return (status & PS2_AUX_DATA) != 0U && response == PS2_MOUSE_ACK;
}

int mouse_initialize(void)
{
    unsigned char config;
    mouse_ready = 0;
    relative_x = 0;
    relative_y = 0;
    packets = 0U;
    left_presses = 0U;
    right_presses = 0U;
    middle_presses = 0U;
    buttons = 0U;
    packet_index = 0U;

    if (!wait_input_empty()) {
        return 0;
    }
    outb(PS2_COMMAND_PORT, PS2_COMMAND_ENABLE_AUX);
    if (!controller_read_config(&config)) {
        return 0;
    }

    /* Enable IRQ12 and the auxiliary clock while preserving other bits. */
    config = (unsigned char)((config | 0x02U) & (unsigned char)~0x20U);
    if (!controller_write_config(config) ||
        !send_auxiliary_command(PS2_MOUSE_ENABLE_STREAMING)) {
        return 0;
    }

    mouse_ready = 1;
    return 1;
}

static int clamp_coordinate(int value)
{
    if (value > MOUSE_COORDINATE_LIMIT) {
        return MOUSE_COORDINATE_LIMIT;
    }
    if (value < -MOUSE_COORDINATE_LIMIT - 1) {
        return -MOUSE_COORDINATE_LIMIT - 1;
    }
    return value;
}

static void count_press(unsigned char newly_pressed, unsigned char mask,
                        volatile unsigned int *counter)
{
    if ((newly_pressed & mask) != 0U && *counter != 0xFFFFFFFFU) {
        ++*counter;
    }
}

void mouse_handle_interrupt(void)
{
    unsigned char value = inb(PS2_DATA_PORT);
    if (!mouse_ready) {
        return;
    }

    if (packet_index == 0U && (value & 0x08U) == 0U) {
        return;
    }
    packet[packet_index++] = value;
    if (packet_index < 3U) {
        return;
    }

    packet_index = 0U;
    unsigned char new_buttons = packet[0] & 0x07U;
    unsigned char pressed = new_buttons & (unsigned char)~buttons;
    int dx = (int)(signed char)packet[1];
    int dy = -(int)(signed char)packet[2];

    relative_x = clamp_coordinate(relative_x + dx);
    relative_y = clamp_coordinate(relative_y + dy);
    buttons = new_buttons;
    count_press(pressed, 0x01U, &left_presses);
    count_press(pressed, 0x02U, &right_presses);
    count_press(pressed, 0x04U, &middle_presses);
    if (packets != 0xFFFFFFFFU) {
        ++packets;
    }
}

void mouse_get_state(struct mouse_state *state)
{
    unsigned int flags;
    if (state == 0) {
        return;
    }

    __asm__ volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    state->available = mouse_ready;
    state->relative_x = relative_x;
    state->relative_y = relative_y;
    state->packets = packets;
    state->left_presses = left_presses;
    state->right_presses = right_presses;
    state->middle_presses = middle_presses;
    state->buttons = buttons;
    __asm__ volatile ("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

