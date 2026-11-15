#ifndef TINYOS_TIMER_H
#define TINYOS_TIMER_H

void timer_initialize(void);
void timer_handle_interrupt(void);
unsigned int timer_ticks(void);
unsigned int timer_uptime_seconds(void);

#endif

