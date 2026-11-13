#include "gdt.h"

struct gdt_entry {
    unsigned short limit_low;
    unsigned short base_low;
    unsigned char base_middle;
    unsigned char access;
    unsigned char granularity;
    unsigned char base_high;
} __attribute__((packed));

struct gdt_pointer {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

/* 32-bit x86 TSS layout. */
struct tss_entry {
    unsigned int previous;
    unsigned int esp0;
    unsigned int ss0;
    unsigned int esp1;
    unsigned int ss1;
    unsigned int esp2;
    unsigned int ss2;
    unsigned int cr3;
    unsigned int eip;
    unsigned int eflags;
    unsigned int eax;
    unsigned int ecx;
    unsigned int edx;
    unsigned int ebx;
    unsigned int esp;
    unsigned int ebp;
    unsigned int esi;
    unsigned int edi;
    unsigned int es;
    unsigned int cs;
    unsigned int ss;
    unsigned int ds;
    unsigned int fs;
    unsigned int gs;
    unsigned int ldt;
    unsigned short trap;
    unsigned short iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6] __attribute__((aligned(8)));
static struct tss_entry tss __attribute__((aligned(16)));
static struct gdt_pointer gdt_ptr;

extern void gdt_load(const struct gdt_pointer *pointer);

static void set_gdt_entry(unsigned int index, unsigned int base,
                          unsigned int limit, unsigned char access,
                          unsigned char flags)
{
    gdt[index].limit_low = (unsigned short)(limit & 0xFFFFU);
    gdt[index].base_low = (unsigned short)(base & 0xFFFFU);
    gdt[index].base_middle = (unsigned char)((base >> 16) & 0xFFU);
    gdt[index].access = access;
    gdt[index].granularity = (unsigned char)(((limit >> 16) & 0x0FU) |
                                             (flags & 0xF0U));
    gdt[index].base_high = (unsigned char)((base >> 24) & 0xFFU);
}

void gdt_initialize(void)
{
    unsigned char *bytes = (unsigned char *)&tss;
    for (unsigned int i = 0; i < sizeof(tss); ++i) {
        bytes[i] = 0U;
    }
    tss.ss0 = 0x10U;
    tss.iomap_base = (unsigned short)sizeof(tss);

    set_gdt_entry(0U, 0U, 0U, 0U, 0U);
    set_gdt_entry(1U, 0U, 0xFFFFFU, 0x9AU, 0xC0U); /* kernel code 0x08 */
    set_gdt_entry(2U, 0U, 0xFFFFFU, 0x92U, 0xC0U); /* kernel data 0x10 */
    set_gdt_entry(3U, 0U, 0xFFFFFU, 0xFAU, 0xC0U); /* user code 0x1B */
    set_gdt_entry(4U, 0U, 0xFFFFFU, 0xF2U, 0xC0U); /* user data 0x23 */
    set_gdt_entry(5U, (unsigned int)&tss,
                  (unsigned int)sizeof(tss) - 1U, 0x89U, 0U); /* TSS 0x28 */

    gdt_ptr.limit = (unsigned short)(sizeof(gdt) - 1U);
    gdt_ptr.base = (unsigned int)&gdt[0];
    gdt_load(&gdt_ptr);
}

void gdt_set_kernel_stack(unsigned int stack_top)
{
    tss.esp0 = stack_top;
    tss.ss0 = 0x10U;
}
