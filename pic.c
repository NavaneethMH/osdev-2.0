#include "pic.h"

static void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void pic_initialize(void)
{
    outb(0x20U, 0x11U);
    outb(0xA0U, 0x11U);
    outb(0x21U, 0x20U);
    outb(0xA1U, 0x28U);
    outb(0x21U, 0x04U);
    outb(0xA1U, 0x02U);
    outb(0x21U, 0x01U);
    outb(0xA1U, 0x01U);
    /* IRQ0/1 plus the IRQ2 cascade; enable only IRQ12 on the slave PIC. */
    outb(0x21U, 0xF8U);
    outb(0xA1U, 0xEFU);
}

void pic_send_eoi(unsigned int irq_vector)
{
    if (irq_vector >= 40U) {
        outb(0xA0U, 0x20U);
    }
    outb(0x20U, 0x20U);
}

