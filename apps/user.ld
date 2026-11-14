OUTPUT_FORMAT(elf32-i386)
OUTPUT_ARCH(i386)
ENTRY(_start)

PHDRS
{
    app PT_LOAD FLAGS(5);
}

SECTIONS
{
    . = 0x40000000;
    .text :
    {
        *(.text*)
        *(.rodata*)
    } :app

    /DISCARD/ :
    {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
