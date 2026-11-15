#include "timer.h"

#define PIT_COMMAND_PORT 0x43U
#define PIT_CHANNEL0_PORT 0x40U
#define PIT_TICKS_PER_SECOND 100U

static volatile unsigned int ticks = 0U;
static volatile unsigned int uptime_seconds = 0U;
static unsigned int ticks_in_second = 0U;

static void outb(unsigned short port, unsigned char value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

void timer_initialize(void)
{
    unsigned int divisor = 11932U;
    outb(PIT_COMMAND_PORT, 0x36U);
    outb(PIT_CHANNEL0_PORT, (unsigned char)(divisor & 0xFFU));
    outb(PIT_CHANNEL0_PORT, (unsigned char)((divisor >> 8) & 0xFFU));
    ticks = 0U;
    uptime_seconds = 0U;
    ticks_in_second = 0U;
}

void timer_handle_interrupt(void)
{
    ++ticks;
    if (++ticks_in_second >= PIT_TICKS_PER_SECOND) {
        ticks_in_second = 0U;
        ++uptime_seconds;
    }
}

unsigned int timer_ticks(void)
{
    return ticks;
}

unsigned int timer_uptime_seconds(void)
{
    return uptime_seconds;
}

