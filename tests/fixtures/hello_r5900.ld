/* Script de linkagem mínimo: programas do EE carregam em 0x00100000. */
ENTRY(_start)
SECTIONS
{
    . = 0x00100000;
    .text : { *(.text) }
    . = ALIGN(128);
    .data : { *(.data) }
    . = ALIGN(16);
    .bss : { *(.bss) }
    /DISCARD/ : { *(.reginfo) *(.MIPS.abiflags) *(.pdr) *(.gnu.attributes) }
}
