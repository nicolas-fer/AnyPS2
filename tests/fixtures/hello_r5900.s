# Programa mínimo do EE usado como fixture dos testes do parser de ELF e
# da CLI. Não depende do PS2SDK: é montado só com GNU binutils
# (-march=r5900). Veja build_fixtures.sh.
#
# Exercita: prólogo/epílogo, chamadas (jal/jalr), desvios likely, MMI de
# 128 bits, FPU (COP1), VU0 em modo macro (COP2), dados, bss e syscall.

        .set    noreorder
        .set    noat

        .text
        .globl  _start
        .type   _start, @function
        .ent    _start
_start:
        lui     $sp, %hi(_stack_top)
        addiu   $sp, $sp, %lo(_stack_top)
        jal     main
        nop
        move    $a0, $v0
        addiu   $v1, $zero, 4          # syscall Exit(a0)
        syscall
        b       .                      # nunca retorna
        nop
        .end    _start
        .size   _start, .-_start

        .globl  main
        .type   main, @function
        .ent    main
main:
        addiu   $sp, $sp, -32
        sd      $ra, 16($sp)
        lui     $a0, %hi(message)
        jal     strlen_simple
        addiu   $a0, $a0, %lo(message) # delay slot
        lui     $t0, %hi(buffer)
        addiu   $t0, $t0, %lo(buffer)
        lq      $t1, 0($t0)            # MMI: carga de 128 bits
        pextlw  $t2, $t1, $t1
        sq      $t2, 16($t0)
        mtc1    $v0, $f0
        cvt.s.w $f1, $f0
        add.s   $f2, $f1, $f1
        qmtc2   $t2, $vf1
        vadd.xyzw $vf2, $vf1, $vf1
        lui     $t9, %hi(callback)
        addiu   $t9, $t9, %lo(callback)
        jalr    $t9
        nop
        ld      $ra, 16($sp)
        jr      $ra
        addiu   $sp, $sp, 32
        .end    main
        .size   main, .-main

        .type   strlen_simple, @function
        .ent    strlen_simple
strlen_simple:
        move    $v0, $zero
1:      lbu     $t0, 0($a0)
        beql    $t0, $zero, 2f
        nop
        addiu   $a0, $a0, 1
        b       1b
        addiu   $v0, $v0, 1
2:      jr      $ra
        nop
        .end    strlen_simple
        .size   strlen_simple, .-strlen_simple

        .type   callback, @function
        .ent    callback
callback:
        jr      $ra
        nop
        .end    callback
        .size   callback, .-callback

        .data
        .globl  message
        .type   message, @object
message:
        .asciz  "Ola, PS2!\n"
        .size   message, .-message

        .bss
        .align  4
        .globl  buffer
        .type   buffer, @object
buffer:
        .space  64
        .size   buffer, 64
        .space  4096
_stack_top:
