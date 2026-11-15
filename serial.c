#include "serial.h"

#define SERIAL_COM1 0x3F8U
#define SERIAL_DATA 0U
#define SERIAL_INTERRUPT_ENABLE 1U
#define SERIAL_FIFO_CONTROL 2U
#define SERIAL_LINE_CONTROL 3U
#define SERIAL_MODEM_CONTROL 4U
#define SERIAL_LINE_STATUS 5U
#define SERIAL_TRANSMIT_EMPTY 0x20U
#define SERIAL_RECEIVE_READY 0x01U
#define SERIAL_POLL_LIMIT 100000U

static int serial_ready = 0;

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

static int transmit(unsigned char value)
{
    for (unsigned int attempt = 0U; attempt < SERIAL_POLL_LIMIT; ++attempt) {
        if ((inb(SERIAL_COM1 + SERIAL_LINE_STATUS) &
             SERIAL_TRANSMIT_EMPTY) != 0U) {
            outb(SERIAL_COM1 + SERIAL_DATA, value);
            return 1;
        }
    }
    return 0;
}

void serial_initialize(void)
{
    serial_ready = 0;
    outb(SERIAL_COM1 + SERIAL_INTERRUPT_ENABLE, 0x00U);
    outb(SERIAL_COM1 + SERIAL_LINE_CONTROL, 0x80U);
    outb(SERIAL_COM1 + SERIAL_DATA, 0x01U); /* 115200 baud divisor. */
    outb(SERIAL_COM1 + SERIAL_INTERRUPT_ENABLE, 0x00U);
    outb(SERIAL_COM1 + SERIAL_LINE_CONTROL, 0x03U); /* 8 data, no parity. */
    outb(SERIAL_COM1 + SERIAL_FIFO_CONTROL, 0xC7U);

    /* Check for a UART with the standard loopback probe before using it. */
    outb(SERIAL_COM1 + SERIAL_MODEM_CONTROL, 0x1EU);
    outb(SERIAL_COM1 + SERIAL_DATA, 0xAEU);
    if ((inb(SERIAL_COM1 + SERIAL_LINE_STATUS) & SERIAL_RECEIVE_READY) != 0U &&
        inb(SERIAL_COM1 + SERIAL_DATA) == 0xAEU) {
        serial_ready = 1;
    }
    outb(SERIAL_COM1 + SERIAL_MODEM_CONTROL, 0x0BU);
}

void serial_write_char(char character)
{
    if (!serial_ready) {
        return;
    }
    if (character == '\b') {
        if (transmit('\b')) {
            (void)transmit(' ');
            (void)transmit('\b');
        }
        return;
    }
    if (character == '\n' && !transmit('\r')) {
        return;
    }
    (void)transmit((unsigned char)character);
}

