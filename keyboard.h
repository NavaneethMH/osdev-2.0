#ifndef TINYOS_KEYBOARD_H
#define TINYOS_KEYBOARD_H

void keyboard_initialize(void);
void keyboard_handle_interrupt(void);
int keyboard_has_input(void);
char keyboard_read_char(void);

#endif

