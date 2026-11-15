#ifndef TINYOS_MOUSE_H
#define TINYOS_MOUSE_H

struct mouse_state {
    int available;
    int relative_x;
    int relative_y;
    unsigned int packets;
    unsigned int left_presses;
    unsigned int right_presses;
    unsigned int middle_presses;
    unsigned char buttons;
};

int mouse_initialize(void);
void mouse_handle_interrupt(void);
void mouse_get_state(struct mouse_state *state);

#endif

