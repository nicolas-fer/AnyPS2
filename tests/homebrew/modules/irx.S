/* IRX do ps2sdk embutidos no ELF (montados com -Wa,-I$PS2SDK/iop/irx). */
    .section .data
    .balign 64
    .globl padman_irx
padman_irx:
    .incbin "padman.irx"
    .globl padman_irx_end
padman_irx_end:
    .balign 64
    .globl usbd_irx
usbd_irx:
    .incbin "usbd.irx"
    .globl usbd_irx_end
usbd_irx_end:
