/* IRX do ps2sdk embutidos no ELF (montados com -Wa,-I$PS2SDK/iop/irx). */
    .section .data
    .balign 64
    .globl freesd_irx
freesd_irx:
    .incbin "freesd.irx"
    .globl freesd_irx_end
freesd_irx_end:
    .balign 64
    .globl audsrv_irx
audsrv_irx:
    .incbin "audsrv.irx"
    .globl audsrv_irx_end
audsrv_irx_end:
