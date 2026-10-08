# Trechos em assembly do teste kpatch: chamadas de syscall por número e os
# dois jeitos de usar ERET que jogos usam (sair de um trecho em modo kernel).
    .set noreorder
    .set noat
    .text

    .globl sys_83
sys_83:                     # syscall 0x83: no HLE, só existe se o programa instalar
    li      $v1, 0x83
    syscall
    jr      $ra
    nop

    .globl sys_84
sys_84:                     # syscall 0x84: redirecionada pelo programa
    li      $v1, 0x84
    syscall
    jr      $ra
    nop

    .globl sys_7f
sys_7f:                     # GetMemorySize
    li      $v1, 0x7f
    syscall
    jr      $ra
    nop

    .globl get_status
get_status:
    mfc0    $v0, $12
    jr      $ra
    nop

# kmode_write(addr, valor): liga ERL (modo kernel), grava e volta com ERET,
# que salta para ErrorEPC (= ra) e desliga ERL.
    .globl kmode_write
kmode_write:
    mfc0    $t0, $12
    ori     $t0, $t0, 4
    mtc0    $t0, $12
    sync.p
    sw      $a1, 0($a0)
    mtc0    $ra, $30
    sync.p
    eret
    jr      $ra             # não executado
    nop

# eret_epc(): com EXL ligado e EPC apontando para o rótulo seguinte, ERET
# continua ali e desliga EXL. Devolve o Status depois do ERET.
    .globl eret_epc
eret_epc:
    mfc0    $t0, $12
    ori     $t0, $t0, 2
    mtc0    $t0, $12
    sync.p
    lui     $t1, %hi(1f)
    addiu   $t1, $t1, %lo(1f)
    mtc0    $t1, $14
    sync.p
    eret
    nop
1:  mfc0    $v0, $12
    jr      $ra
    nop
