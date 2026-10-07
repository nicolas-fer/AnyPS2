# Casos de borda do R5900 escritos à mão (montados com -march=r5900).
# Cada função é chamada por ee_tests.c, que compara o resultado com uma
# implementação de referência em C.

        .set    noreorder
        .set    noat
        .text

# ---------------------------------------------------------------------------
# Controle de fluxo
# ---------------------------------------------------------------------------

# int cf_likely_not_taken(void): beql não tomado anula o delay slot -> 1
        .globl  cf_likely_not_taken
cf_likely_not_taken:
        li      $v0, 1
        li      $t0, 5
        beql    $t0, $zero, 1f
        li      $v0, 99                 # anulado
1:      jr      $ra
        nop

# int cf_likely_taken(void): beql tomado executa o delay slot -> 7
        .globl  cf_likely_taken
cf_likely_taken:
        li      $v0, 1
        beql    $zero, $zero, 1f
        li      $v0, 7                  # executado
        li      $v0, 99
1:      jr      $ra
        nop

# int cf_delay_slot_is_target(int x): o delay slot também é alvo de desvio.
# x == 0: entra pelo slot (v0 = 0 + 100 = 100); senão: 10 + 100 = 110
        .globl  cf_delay_slot_is_target
cf_delay_slot_is_target:
        beq     $a0, $zero, 2f
        li      $v0, 0
        li      $v0, 10
        b       3f
2:      addiu   $v0, $v0, 100           # delay slot do "b" e alvo do beq
        jr      $ra
        nop
3:      jr      $ra
        nop

# int cf_branch_reads_before_slot(void): a condição é avaliada antes do slot
# alterar o registrador -> 1
        .globl  cf_branch_reads_before_slot
cf_branch_reads_before_slot:
        li      $t0, 0
        li      $v0, 0
        beq     $t0, $zero, 1f
        li      $t0, 1                  # muda t0 no slot, mas o desvio já foi decidido
        li      $v0, 99
1:      addu    $v0, $v0, $t0
        jr      $ra
        nop

# int cf_jr_reads_before_slot(void): jr usa o alvo lido antes do slot -> 5
        .globl  cf_jr_reads_before_slot
cf_jr_reads_before_slot:
        la      $t0, 1f
        jr      $t0
        move    $t0, $zero              # não afeta o salto
        li      $v0, 99
        jr      $ra
        nop
1:      li      $v0, 5
        jr      $ra
        nop

# u32 cf_bal_pc(void): bal para obter o PC; retorna ra - endereço do bal = 8
        .globl  cf_bal_pc
cf_bal_pc:
        move    $t1, $ra
        la      $t2, 1f
1:      bal     2f
        nop
2:      subu    $v0, $ra, $t2
        jr      $t1
        nop

# int cf_jalr_rd(void): jalr com rd = t3; o destino retorna por jr t3 -> 21
        .globl  cf_jalr_rd
cf_jalr_rd:
        la      $t0, 1f
        jalr    $t3, $t0
        li      $v0, 20
        jr      $ra
        nop
1:      jr      $t3
        addiu   $v0, $v0, 1

# int cf_movz_movn(int a, int b): (a == 0 ? 11 : 0) + (b != 0 ? 22 : 0)
        .globl  cf_movz_movn
cf_movz_movn:
        li      $t0, 11
        li      $t1, 22
        move    $v0, $zero
        move    $t2, $zero
        movz    $t2, $t0, $a0
        movn    $v0, $t1, $a1
        jr      $ra
        addu    $v0, $v0, $t2

# ---------------------------------------------------------------------------
# Acessos desalinhados: load/store de 4 e 8 bytes em src+off
# ---------------------------------------------------------------------------

# u32 mem_lw_unaligned(const u8* p)
        .globl  mem_lw_unaligned
mem_lw_unaligned:
        lwl     $v0, 3($a0)
        lwr     $v0, 0($a0)
        jr      $ra
        nop

# u64 mem_ld_unaligned(const u8* p, u64* out)
        .globl  mem_ld_unaligned
mem_ld_unaligned:
        ldl     $t0, 7($a0)
        ldr     $t0, 0($a0)
        sd      $t0, 0($a1)
        jr      $ra
        nop

# void mem_sw_unaligned(u8* p, u32 v)
        .globl  mem_sw_unaligned
mem_sw_unaligned:
        swl     $a1, 3($a0)
        swr     $a1, 0($a0)
        jr      $ra
        nop

# void mem_sd_unaligned(u8* p, const u64* v)
        .globl  mem_sd_unaligned
mem_sd_unaligned:
        ld      $t0, 0($a1)
        sdl     $t0, 7($a0)
        sdr     $t0, 0($a0)
        jr      $ra
        nop

# ---------------------------------------------------------------------------
# MMI: void mmi_X(const u128* a, const u128* b, u128* out)
# ---------------------------------------------------------------------------

        .macro  MMI3 name, insn
        .globl  mmi_\name
mmi_\name:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        \insn   $t2, $t0, $t1
        jr      $ra
        sq      $t2, 0($a2)
        .endm

        .macro  MMI2 name, insn
        .globl  mmi_\name
mmi_\name:
        lq      $t1, 0($a1)
        \insn   $t2, $t1
        jr      $ra
        sq      $t2, 0($a2)
        .endm

        MMI3    paddw, paddw
        MMI3    psubh, psubh
        MMI3    paddsh, paddsh
        MMI3    psubsb, psubsb
        MMI3    paddub, paddub
        MMI3    psubuw, psubuw
        MMI3    pcgtb, pcgtb
        MMI3    pceqh, pceqh
        MMI3    pmaxw, pmaxw
        MMI3    pminh, pminh
        MMI3    pextlw, pextlw
        MMI3    pextuh, pextuh
        MMI3    pextlb, pextlb
        MMI3    ppach, ppach
        MMI3    ppacb, ppacb
        MMI3    pinth, pinth
        MMI3    pcpyld, pcpyld
        MMI3    pcpyud, pcpyud
        MMI3    pand, pand
        MMI3    pnor, pnor
        MMI3    padsbh, padsbh
        MMI2    pabsw, pabsw
        MMI2    pext5, pext5
        MMI2    ppac5, ppac5
        MMI2    pexeh, pexeh
        MMI2    prevh, prevh
        MMI2    prot3w, prot3w
        MMI2    pexcw, pexcw
        MMI2    pcpyh, pcpyh

# Shifts paralelos com quantidade fixa
        .globl  mmi_psllh5
mmi_psllh5:
        lq      $t1, 0($a1)
        psllh   $t2, $t1, 5
        jr      $ra
        sq      $t2, 0($a2)

        .globl  mmi_psraw7
mmi_psraw7:
        lq      $t1, 0($a1)
        psraw   $t2, $t1, 7
        jr      $ra
        sq      $t2, 0($a2)

# psravw: desloca rt pelos 5 bits baixos das palavras 0 e 2 de rs
        .globl  mmi_psravw
mmi_psravw:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        psravw  $t2, $t1, $t0
        jr      $ra
        sq      $t2, 0($a2)

# qfsrv com SA = 5 bytes (mtsab)
        .globl  mmi_qfsrv5
mmi_qfsrv5:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        li      $t3, 5
        mtsab   $t3, 0
        qfsrv   $t2, $t0, $t1
        jr      $ra
        sq      $t2, 0($a2)

# pmultw: out[0] = rd, out[1] = HI, out[2] = LO
        .globl  mmi_pmultw
mmi_pmultw:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        pmultw  $t2, $t0, $t1
        sq      $t2, 0($a2)
        pmfhi   $t2
        sq      $t2, 16($a2)
        pmflo   $t2
        jr      $ra
        sq      $t2, 32($a2)

# pmaddh com HI/LO zerados antes: out[0] = rd, out[1] = HI, out[2] = LO
        .globl  mmi_pmaddh
mmi_pmaddh:
        pmthi   $zero
        pmtlo   $zero
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        pmaddh  $t2, $t0, $t1
        pmaddh  $t2, $t0, $t1           # acumula duas vezes
        sq      $t2, 0($a2)
        pmfhi   $t2
        sq      $t2, 16($a2)
        pmflo   $t2
        jr      $ra
        sq      $t2, 32($a2)

# pdivw: out[0] = HI (restos), out[1] = LO (quocientes)
        .globl  mmi_pdivw
mmi_pdivw:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        pdivw   $t0, $t1
        pmfhi   $t2
        sq      $t2, 0($a2)
        pmflo   $t2
        jr      $ra
        sq      $t2, 16($a2)

# pmfhl.*: carrega HI/LO de a e b; out[0..4] = lw, uw, slw, lh, sh
        .globl  mmi_pmfhl
mmi_pmfhl:
        lq      $t0, 0($a0)
        lq      $t1, 0($a1)
        pmthi   $t0
        pmtlo   $t1
        pmfhl.lw $t2
        sq      $t2, 0($a2)
        pmfhl.uw $t2
        sq      $t2, 16($a2)
        pmfhl.slw $t2
        sq      $t2, 32($a2)
        pmfhl.lh $t2
        sq      $t2, 48($a2)
        pmfhl.sh $t2
        jr      $ra
        sq      $t2, 64($a2)

# plzcw: out = plzcw(a) (palavras 0 e 1)
        .globl  mmi_plzcw
mmi_plzcw:
        lq      $t0, 0($a0)
        pcpyld  $t2, $zero, $zero
        plzcw   $t2, $t0
        jr      $ra
        sq      $t2, 0($a2)

# Multiplicação de 3 operandos e pipeline 1:
# void mul3(int a, int b, int* out): out[0] = mult rd, out[1] = mflo1 de mult1,
# out[2] = mfhi1 (resto) e out[3] = mflo1 (quociente) de div1, out[4] = madd
        .globl  mul3
mul3:
        mult    $t0, $a0, $a1
        sw      $t0, 0($a2)
        mult1   $zero, $a0, $a1
        mflo1   $t1
        sw      $t1, 4($a2)
        div1    $zero, $a0, $a1
        mfhi1   $t1
        sw      $t1, 8($a2)
        mflo1   $t1
        sw      $t1, 12($a2)
        mtlo    $zero
        mthi    $zero
        madd    $t1, $a0, $a1
        madd    $t1, $a0, $a1
        jr      $ra
        sw      $t1, 16($a2)

# ---------------------------------------------------------------------------
# FPU do R5900 (sem IEEE): u32 fpu_X(u32 a_bits, u32 b_bits)
# ---------------------------------------------------------------------------

        .macro  FPU3 name, insn
        .globl  fpu_\name
fpu_\name:
        mtc1    $a0, $f1
        mtc1    $a1, $f2
        \insn   $f0, $f1, $f2
        jr      $ra
        mfc1    $v0, $f0
        .endm

        FPU3    add, add.s
        FPU3    mul, mul.s
        FPU3    div, div.s
        FPU3    max, max.s
        FPU3    min, min.s

# sqrt.s fd, ft (o operando é ft no R5900)
        .globl  fpu_sqrt
fpu_sqrt:
        mtc1    $a0, $f2
        sqrt.s  $f0, $f2
        jr      $ra
        mfc1    $v0, $f0

        .globl  fpu_cvtw
fpu_cvtw:
        mtc1    $a0, $f1
        cvt.w.s $f0, $f1
        jr      $ra
        mfc1    $v0, $f0

# madd.s com acumulador: acc = a*b (mula.s), fd = acc + a*b
        .globl  fpu_madd_acc
fpu_madd_acc:
        mtc1    $a0, $f1
        mtc1    $a1, $f2
        mula.s  $f1, $f2
        madd.s  $f0, $f1, $f2
        jr      $ra
        mfc1    $v0, $f0

# int fpu_lt(a, b): c.lt.s + bc1t
        .globl  fpu_lt
fpu_lt:
        mtc1    $a0, $f1
        mtc1    $a1, $f2
        c.lt.s  $f1, $f2
        bc1t    1f
        li      $v0, 1
        li      $v0, 0
1:      jr      $ra
        nop
