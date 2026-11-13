#ifndef TINYOS_GDT_H
#define TINYOS_GDT_H

void gdt_initialize(void);
void gdt_set_kernel_stack(unsigned int stack_top);

#endif
