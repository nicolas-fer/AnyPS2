#!/usr/bin/env python3
"""Gera tests/unit/instruction_table.inc: uma codificação de referência por
instrução do R5900 (todas as entradas de opcodes.def), com o texto esperado
confirmado pelo GNU objdump (-m mips:5900 -M no-aliases).

As codificações são montadas campo a campo a partir dos mapas de opcode do
"EE Core Instruction Set Manual", de forma independente das tabelas do
decodificador. O texto do objdump passa pelas mesmas normalizações
documentadas em tests/unit/test_golden.cpp.

Uso: gen_instruction_table.py [--objdump mipsel-linux-gnu-objdump]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
def R(op=0, rs=0, rt=0, rd=0, sa=0, f=0): return (op<<26)|(rs<<21)|(rt<<16)|(rd<<11)|(sa<<6)|f
def I(op, rs, rt, imm): return (op<<26)|(rs<<21)|(rt<<16)|(imm&0xFFFF)
def JJ(op, target): return (op<<26)|((target>>2)&0x3FFFFFF)
A,B,C,S=4,5,6,7   # a0,a1,a2 e shamt
E={}
prim={'J':2,'JAL':3}
E['J']=JJ(2,0x00123450); E['JAL']=JJ(3,0x00200000)
for n,o in [('BEQ',4),('BNE',5),('BEQL',20),('BNEL',21)]: E[n]=I(o,A,B,0x0010)
for n,o in [('BLEZ',6),('BGTZ',7),('BLEZL',22),('BGTZL',23)]: E[n]=I(o,A,0,0xFFFC)
for n,o in [('ADDI',8),('ADDIU',9),('SLTI',10),('SLTIU',11),('DADDI',24),('DADDIU',25)]: E[n]=I(o,A,B,0xFFF0)
for n,o in [('ANDI',12),('ORI',13),('XORI',14)]: E[n]=I(o,A,B,0xBEEF)
E['LUI']=I(15,0,B,0x8000)
mem=[('LDL',26),('LDR',27),('LQ',30),('SQ',31),('LB',32),('LH',33),('LWL',34),('LW',35),('LBU',36),('LHU',37),('LWR',38),('LWU',39),('SB',40),('SH',41),('SWL',42),('SW',43),('SDL',44),('SDR',45),('SWR',46),('LD',55),('SD',63)]
for n,o in mem: E[n]=I(o,29,B,0xFFE0 if n!='LQ' else 0x0040)
E['CACHE']=I(47,A,0x18,0x0040); E['PREF']=I(51,A,0,0x0010)
E['LWC1']=I(49,A,3,0x0008); E['SWC1']=I(57,A,3,0x000C); E['LQC2']=I(54,A,3,0x0010); E['SQC2']=I(62,A,3,0xFFF0)
# SPECIAL
for n,f in [('SLL',0),('SRL',2),('SRA',3),('DSLL',56),('DSRL',58),('DSRA',59),('DSLL32',60),('DSRL32',62),('DSRA32',63)]: E[n]=R(0,0,B,C,S,f)
for n,f in [('SLLV',4),('SRLV',6),('SRAV',7),('DSLLV',20),('DSRLV',22),('DSRAV',23)]: E[n]=R(0,A,B,C,0,f)
E['JR']=R(0,31,0,0,0,8); E['JALR']=R(0,A,0,C,0,9)
for n,f in [('MOVZ',10),('MOVN',11),('MULT',24),('MULTU',25),('ADD',32),('ADDU',33),('SUB',34),('SUBU',35),('AND',36),('OR',37),('XOR',38),('NOR',39),('SLT',42),('SLTU',43),('DADD',44),('DADDU',45),('DSUB',46),('DSUBU',47)]: E[n]=R(0,A,B,C,0,f)
E['SYSCALL']=R(0,0,0,0,0,12); E['BREAK']=(0x7<<16)|13; E['SYNC']=15
for n,f in [('MFHI',16),('MFLO',18),('MFSA',40)]: E[n]=R(0,0,0,C,0,f)
for n,f in [('MTHI',17),('MTLO',19),('MTSA',41)]: E[n]=R(0,A,0,0,0,f)
for n,f in [('DIV',26),('DIVU',27)]: E[n]=R(0,A,B,0,0,f)
for n,f in [('TGE',48),('TGEU',49),('TLT',50),('TLTU',51),('TEQ',52),('TNE',54)]: E[n]=R(0,A,B,0,0,f)
# REGIMM
for n,rt in [('BLTZ',0),('BGEZ',1),('BLTZL',2),('BGEZL',3),('BLTZAL',16),('BGEZAL',17),('BLTZALL',18),('BGEZALL',19)]: E[n]=I(1,A,rt,0x0004)
for n,rt in [('TGEI',8),('TGEIU',9),('TLTI',10),('TLTIU',11),('TEQI',12),('TNEI',14),('MTSAB',24),('MTSAH',25)]: E[n]=I(1,A,rt,0x0002)
# MMI
M=28
for n,f in [('MADD',0),('MADDU',1),('MULT1',24),('MULTU1',25),('MADD1',32),('MADDU1',33)]: E[n]=R(M,A,B,C,0,f)
E['PLZCW']=R(M,A,0,C,0,4)
for n,f in [('MFHI1',16),('MFLO1',18)]: E[n]=R(M,0,0,C,0,f)
for n,f in [('MTHI1',17),('MTLO1',19)]: E[n]=R(M,A,0,0,0,f)
for n,f in [('DIV1',26),('DIVU1',27)]: E[n]=R(M,A,B,0,0,f)
for n,f in [('PSLLH',52),('PSRLH',54),('PSRAH',55),('PSLLW',60),('PSRLW',62),('PSRAW',63)]: E[n]=R(M,0,B,C,S,f)
mmi0=['PADDW','PSUBW','PCGTW','PMAXW','PADDH','PSUBH','PCGTH','PMAXH','PADDB','PSUBB','PCGTB',None,None,None,None,None,'PADDSW','PSUBSW','PEXTLW','PPACW','PADDSH','PSUBSH','PEXTLH','PPACH','PADDSB','PSUBSB','PEXTLB','PPACB',None,None,'PEXT5','PPAC5']
mmi1=[None,'PABSW','PCEQW','PMINW','PADSBH','PABSH','PCEQH','PMINH',None,None,'PCEQB',None,None,None,None,None,'PADDUW','PSUBUW','PEXTUW',None,'PADDUH','PSUBUH','PEXTUH',None,'PADDUB','PSUBUB','PEXTUB','QFSRV',None,None,None,None]
mmi2=['PMADDW',None,'PSLLVW','PSRLVW','PMSUBW',None,None,None,'PMFHI','PMFLO','PINTH',None,'PMULTW','PDIVW','PCPYLD',None,'PMADDH','PHMADH','PAND','PXOR','PMSUBH','PHMSBH',None,None,None,None,'PEXEH','PREVH','PMULTH','PDIVBW','PEXEW','PROT3W']
mmi3=['PMADDUW',None,None,'PSRAVW',None,None,None,None,'PMTHI','PMTLO','PINTEH',None,'PMULTUW','PDIVUW','PCPYUD',None,None,None,'POR','PNOR',None,None,None,None,None,None,'PEXCH','PCPYH',None,None,'PEXCW',None]
RT_ONLY={'PEXT5','PPAC5','PABSW','PABSH','PEXEH','PREVH','PEXEW','PROT3W','PEXCH','PCPYH','PEXCW'}
RD_ONLY={'PMFHI','PMFLO'}; RS_ONLY={'PMTHI','PMTLO'}; RSRT={'PDIVW','PDIVBW','PDIVUW'}
for tab,f in [(mmi0,8),(mmi1,40),(mmi2,9),(mmi3,41)]:
    for sa,n in enumerate(tab):
        if not n: continue
        if n in RT_ONLY: E[n]=R(M,0,B,C,sa,f)
        elif n in RD_ONLY: E[n]=R(M,0,0,C,sa,f)
        elif n in RS_ONLY: E[n]=R(M,A,0,0,sa,f)
        elif n in RSRT: E[n]=R(M,A,B,0,sa,f)
        else: E[n]=R(M,A,B,C,sa,f)
for sa,n in enumerate(['PMFHL_LW','PMFHL_UW','PMFHL_SLW','PMFHL_LH','PMFHL_SH']): E[n]=R(M,0,0,C,sa,48)
E['PMTHL_LW']=R(M,A,0,0,0,49)
# COP0
E['MFC0']=R(16,0,B,12,0,0); E['MTC0']=R(16,4,B,14,0,0)
for i,n in enumerate(['MFBPC',None,'MFIAB','MFIABM','MFDAB','MFDABM','MFDVB','MFDVBM']):
    if n: E[n]=R(16,0,B,24,0,i); E['MT'+n[2:]]=R(16,4,B,24,0,i)
E['MFPS']=R(16,0,B,25,0,0); E['MFPC']=R(16,0,B,25,0,3); E['MTPS']=R(16,4,B,25,0,0); E['MTPC']=R(16,4,B,25,0,1)
for i,n in enumerate(['BC0F','BC0T','BC0FL','BC0TL']): E[n]=I(16,8,i,0x0003)
for n,f in [('TLBR',1),('TLBWI',2),('TLBWR',6),('TLBP',8),('ERET',24),('EI',56),('DI',57)]: E[n]=R(16,16,0,0,0,f)
# COP1
E['MFC1']=R(17,0,B,3); E['CFC1']=R(17,2,B,31); E['MTC1']=R(17,4,B,3); E['CTC1']=R(17,6,B,31)
for i,n in enumerate(['BC1F','BC1T','BC1FL','BC1TL']): E[n]=I(17,8,i,0xFFFF)
FD,FS,FT=1,2,3
def S_(f,fs=FS,ft=FT,fd=FD): return R(17,16,ft,fs,fd,f)
for n,f in [('ADD_S',0),('SUB_S',1),('MUL_S',2),('DIV_S',3),('RSQRT_S',22),('MADD_S',28),('MSUB_S',29),('MAX_S',40),('MIN_S',41)]: E[n]=S_(f)
E['SQRT_S']=S_(4,fs=0)
for n,f in [('ABS_S',5),('MOV_S',6),('NEG_S',7),('CVT_W_S',36)]: E[n]=S_(f,ft=0)
for n,f in [('ADDA_S',24),('SUBA_S',25),('MULA_S',26),('MADDA_S',30),('MSUBA_S',31),('C_F_S',48),('C_EQ_S',50),('C_LT_S',52),('C_LE_S',54)]: E[n]=S_(f,fd=0)
E['CVT_S_W']=R(17,20,0,FS,FD,32)
# COP2
E['QMFC2']=R(18,1,B,3,0,0); E['QMTC2']=R(18,5,B,3,0,1); E['CFC2']=R(18,2,B,16,0,1); E['CTC2']=R(18,6,B,27,0,0)
for i,n in enumerate(['BC2F','BC2T','BC2FL','BC2TL']): E[n]=I(18,8,i,0x0001)
def V1(f,dest=0xF,ft=3,fs=2,fd=1): return (18<<26)|(1<<25)|(dest<<21)|(ft<<16)|(fs<<11)|(fd<<6)|f
def V2(idx,dest=0xF,ft=3,fs=2): return (18<<26)|(1<<25)|(dest<<21)|(ft<<16)|(fs<<11)|((idx>>2)<<6)|0x3C|(idx&3)
for n,base,bc in [('VADDbc',0,0),('VSUBbc',4,1),('VMADDbc',8,2),('VMSUBbc',12,3),('VMAXbc',16,0),('VMINIbc',20,1),('VMULbc',24,2)]: E[n]=V1(base+bc)
for n,f in [('VMULq',28),('VMAXi',29),('VMULi',30),('VMINIi',31),('VADDq',32),('VMADDq',33),('VADDi',34),('VMADDi',35),('VSUBq',36),('VMSUBq',37),('VSUBi',38),('VMSUBi',39)]: E[n]=V1(f,ft=0,dest=0xE)
for n,f in [('VADD',40),('VMADD',41),('VMUL',42),('VMAX',43),('VSUB',44),('VMSUB',45),('VMINI',47)]: E[n]=V1(f,dest=0x9)
E['VOPMSUB']=V1(46,dest=0xE)
for n,f in [('VIADD',48),('VISUB',49),('VIAND',52),('VIOR',53)]: E[n]=V1(f,dest=0)
E['VIADDI']=V1(50,dest=0,fd=0x1D)  # imm5 = -3
E['VCALLMS']=(18<<26)|(1<<25)|(0x40<<6)|56  # 0x200
E['VCALLMSR']=V1(57,dest=0,ft=0,fs=27,fd=0)
for n,b in [('VADDAbc',0),('VSUBAbc',4),('VMADDAbc',8),('VMSUBAbc',12),('VMULAbc',24)]: E[n]=V2(b+3,dest=0x6)
for i,n in enumerate(['VITOF0','VITOF4','VITOF12','VITOF15','VFTOI0','VFTOI4','VFTOI12','VFTOI15']): E[n]=V2(16+i)
for n,idx in [('VMULAq',28),('VMULAi',30),('VADDAq',32),('VMADDAq',33),('VADDAi',34),('VMADDAi',35),('VSUBAq',36),('VMSUBAq',37),('VSUBAi',38),('VMSUBAi',39)]: E[n]=V2(idx,ft=0)
E['VABS']=V2(29); E['VCLIPw']=V2(31,dest=0xE)
for n,idx in [('VADDA',40),('VMADDA',41),('VMULA',42),('VSUBA',44),('VMSUBA',45)]: E[n]=V2(idx,dest=0x3)
E['VOPMULA']=V2(46,dest=0xE); E['VNOP']=V2(47,dest=0,ft=0,fs=0)
E['VMOVE']=V2(48); E['VMR32']=V2(49)
for n,idx in [('VLQI',52),('VSQI',53),('VLQD',54),('VSQD',55)]: E[n]=V2(idx,dest=0xF)
E['VDIV']=V2(56,dest=(2<<2)|1)  # ftf=z fsf=y
E['VSQRT']=V2(57,dest=(3<<2)|1,fs=0)   # ftf=w (fsf=1 como o GNU as gera)
E['VRSQRT']=V2(58,dest=(0<<2)|3)
E['VWAITQ']=V2(59,dest=0,ft=0,fs=0)
E['VMTIR']=V2(60,dest=2)  # fsf=z
E['VMFIR']=V2(61,dest=0xF)
E['VILWR']=V2(62,dest=0x8); E['VISWR']=V2(63,dest=0x1)
E['VRNEXT']=V2(64,dest=0xF,fs=0); E['VRGET']=V2(65,dest=0x8,fs=0)
E['VRINIT']=V2(66,dest=1,ft=0); E['VRXOR']=V2(67,dest=3,ft=0)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--objdump", default="mipsel-linux-gnu-objdump")
    args = ap.parse_args()
    defs = open(os.path.join(ROOT, "recompiler", "include", "anyps2", "r5900", "opcodes.def"),
                encoding="utf-8").read()
    ops = [o for o in re.findall(r"^ANYPS2_OP\(([A-Za-z0-9_]+),", defs, re.M) if o != "Invalid"]
    missing = [o for o in ops if o not in E]
    if missing:
        sys.exit("sem codificação de referência para: " + ", ".join(missing))
    base = 0x00200000
    words = [E[o] for o in ops]
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "t.bin")
        with open(path, "wb") as f:
            f.write(b"".join(struct.pack("<I", w) for w in words))
        out = subprocess.run([args.objdump, "-D", "-z", "-b", "binary", "-m", "mips:5900", "-EL",
                              "-M", "no-aliases", "--adjust-vma=%#x" % base, path],
                             check=True, capture_output=True, text=True).stdout
    text = {}
    for line in out.splitlines():
        p = line.split("\t")
        if len(p) >= 3 and p[0].strip().endswith(":"):
            text[int(p[0].strip()[:-1], 16)] = "\t".join(p[2:]).rstrip()
    rows = []
    for i, op in enumerate(ops):
        addr = base + 4 * i
        t = text[addr]
        m = t.split("\t")[0]
        rest = t[len(m) + 1:]
        if op != "SQRT_S" and (t.startswith(".word") or re.match(r"c[0-3]\t", t)):
            sys.exit("objdump rejeitou a codificação de referência de %s (%#x)" % (op, words[i]))
        if m == "trunc.w.s":
            t = "cvt.w.s\t" + rest
        if m.startswith("vadda.") or m.startswith("vmsuba."):
            p = rest.split(",")
            t = m + "\t" + ",".join([p[0], p[2], p[1]])
        if op == "SQRT_S":  # binutils lê fs; o R5900 lê ft (ver test_golden.cpp)
            t = "sqrt.s\t$f%d,$f%d" % ((words[i] >> 6) & 31, (words[i] >> 16) & 31)
        rows.append('    {Op::%s, 0x%08X, 0x%08X, "%s"},' % (op, words[i], addr,
                                                           t.replace("\t", "\\t")))
    dst = os.path.join(ROOT, "tests", "unit", "instruction_table.inc")
    with open(dst, "w", newline="\n") as f:
        f.write("// Gerado por tests/scripts/gen_instruction_table.py — não edite à mão.\n")
        f.write("// {Op, palavra, endereço, texto esperado}\n")
        f.write("\n".join(rows) + "\n")
    print("%d instruções gravadas em %s" % (len(rows), dst))


if __name__ == "__main__":
    main()
