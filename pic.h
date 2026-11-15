#ifndef TINYOS_PIC_H
#define TINYOS_PIC_H

void pic_initialize(void);
void pic_send_eoi(unsigned int irq_vector);

#endif

