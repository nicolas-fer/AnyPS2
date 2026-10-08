/* O execchild.elf embutido (montado com -Wa,-I/src: a raiz dos homebrews). */
    .section .data
    .balign 64
    .globl child_elf
child_elf:
    .incbin "execchild/execchild.elf"
