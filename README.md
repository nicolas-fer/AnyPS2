# AnyPS2

Recompilador estático de PlayStation 2 para PC (Windows e Linux), no
espírito do [AnyPS5](https://github.com/boykopovar/AnyPS5) e do N64Recomp.

O PS5 e o PC compartilham a arquitetura x86-64, então o AnyPS5 consegue
"relinkar" o executável. No PS2 a CPU principal é um **MIPS R5900 (Emotion
Engine)**, então o código precisa ser **traduzido**:

```
ELF do PS2  ──▶  C++ gerado  ──▶  compilador nativo  ──▶  executável PC
                     │
                     └── linka o runtime do AnyPS2 (kernel do EE em HLE,
                         memória, GS, VU, IOP, SPU2, pad, CDVD...)
```

> **Status: Fase 1 de 7 concluída.** Ainda não roda nenhum programa do PS2.
> Veja o [PLANO.md](PLANO.md) para o roteiro completo.

## O que funciona hoje (Fase 1)

- **Parser de ELF32** (MIPS little-endian): cabeçalho, program headers,
  seções, símbolos, relocações REL/RELA, numeração estendida de seções,
  reconhecimento de módulos IRX do IOP e da marca R5900 em `e_flags`. Toda
  entrada é validada; arquivos malformados geram erro dizendo o campo e o
  offset (testado com 20 mil mutações aleatórias, limpo sob ASan/UBSan).
- **Visão de memória virtual** do ELF (segmentos `PT_LOAD`, bss lido como
  zero) e busca de símbolos por nome e por endereço.
- **Decodificador completo do R5900**: 373 instruções numa tabela única
  (`recompiler/include/anyps2/r5900/opcodes.def`):
  - MIPS III/IV na forma que o EE implementa (sem LL/SC, DMULT/DDIV, LDC1...,
    que retornam inválido);
  - MMI de 128 bits (MMI0–MMI3, PMFHL/PMTHL), pipeline 1 (MULT1, DIV1, ...),
    LQ/SQ, MFSA/MTSA;
  - COP0 (incluindo registradores de debug e contadores de performance);
  - COP1 do R5900 (ADDA/MADD/MSUB..., MAX/MIN, RSQRT, C.cond);
  - COP2 / VU0 em modo macro (Special1 e Special2, QMFC2/QMTC2 com
    interlock, VCALLMS...).
- **Metadados para o gerador de código**: desvio/salto/link/likely/delay
  slot, alvos estáticos, loads/stores com tamanho, traps, exceções, ERET,
  instruções privilegiadas e detecção de codificações não canônicas (bits
  reservados ligados — útil para perceber dados confundidos com código).
- **Disassembler** na sintaxe do GNU objdump (`-m mips:5900 -M no-aliases`).
- **CLI `anyps2`** com `info`, `symbols`, `disasm` e `disasm-bin`.

### Como o decodificador é validado

1. **Uma codificação de referência por instrução** (`tests/unit/instruction_table.inc`),
   montada campo a campo a partir dos mapas de opcode do manual do EE; um
   teste garante que nenhuma instrução do enum fica sem referência.
2. **Oráculo diferencial contra o GNU binutils**, que tem uma implementação
   independente do conjunto do EE: um golden versionado de 13,5 mil
   palavras e, no CI, ~1,6 milhão de palavras novas a cada execução.
   Localmente já foram verificadas 8 milhões de palavras sem divergência.
3. Testes de semântica (alvos de desvio/salto, tamanhos de acesso, flags),
   de palavras inválidas e de robustez (2 milhões de palavras aleatórias).

As divergências em relação ao binutils são intencionais e documentadas em
`tests/unit/test_golden.cpp`:

| Caso | binutils | AnyPS2 | Por quê |
|---|---|---|---|
| `SQRT.S` | lê o operando de `fs` (bits 15..11) | lê de `ft` (bits 20..16) | É o que o hardware faz (manual do EE; comportamento validado do PCSX2) |
| `CVT.W.S` | `trunc.w.s` | `cvt.w.s` | Nome do manual da Sony (no R5900 a conversão sempre trunca) |
| `VADDA`/`VMSUBA` | imprime `ft` antes de `fs` | `ACC,fs,ft` | Consistência com todas as outras instruções com ACC |
| `VSQRT` | só aceita `fsf = 1` | `fsf` ignorado | Campo não usado pelo hardware |
| JALX, CFC0/CTC0, WAIT, formatos D/L e várias funções de COP1 | aceita | inválido | Não existem no R5900 |
| `dli`, `neg`, `negu`... | aliases mesmo com `no-aliases` | `ori rt,zero,imm`, `sub rd,zero,rt`... | Forma real da instrução |

## Compilando

Requisitos: CMake ≥ 3.20 e um compilador C++20 (MSVC 2022, GCC ≥ 11 ou
Clang ≥ 14). A Fase 1 não tem dependências externas (o SDL entra na Fase 4).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Opções: `-DANYPS2_WARNINGS_AS_ERRORS=ON` (usado no CI),
`-DANYPS2_BUILD_TESTS=OFF`.

## Usando a CLI

```sh
anyps2 info     jogo.elf                 # cabeçalho, segmentos, seções, símbolos
anyps2 symbols  jogo.elf --functions     # lista funções
anyps2 disasm   jogo.elf --symbol main   # desmonta uma função
anyps2 disasm   jogo.elf --range 0x100000 0x100100 --mark-noncanonical
anyps2 disasm-bin dump.bin --base 0x00100000
```

Exemplo (fixture `tests/fixtures/hello_r5900.elf`):

```
00100024 <main>:
  100024:	27bdffe0 	addiu	sp,sp,-32
  100028:	ffbf0010 	sd	ra,16(sp)
  ...
  100030:	0c04001f 	jal	0x10007c <strlen_simple>
  ...
  100040:	79090000 	lq	t1,0(t0)
  100044:	71295488 	pextlw	t2,t1,t1
  ...
  10005c:	4be108a8 	vadd.xyzw	$vf2xyzw,$vf1xyzw,$vf1xyzw
```

`anyps2 recomp` existe, mas ainda responde com erro explícito: o gerador de
C++ é a Fase 2.

## Estrutura do repositório

```
common/       utilitários compartilhados (erros, leitura little-endian)
recompiler/   biblioteca: ELF, decodificador/disassembler R5900 (e, na Fase 2,
              análise de funções e gerador de C++)
runtime/      biblioteca linkada pelo código gerado (começa na Fase 2)
tools/        CLI anyps2
tests/        framework mínimo, testes unitários, golden do objdump,
              fixtures e scripts (objdump_oracle.py, gen_instruction_table.py)
PLANO.md      roteiro detalhado das 7 fases
```

### Regenerando os arquivos derivados

Requer `binutils-mipsel-linux-gnu` (Debian/Ubuntu):

```sh
python3 tests/scripts/objdump_oracle.py golden          # tests/data/r5900_golden.txt
python3 tests/scripts/gen_instruction_table.py          # tests/unit/instruction_table.inc
tests/fixtures/build_fixtures.sh                        # fixture ELF + listagem esperada
python3 tests/scripts/objdump_oracle.py check --tests build/tests/anyps2_tests
```

## O que falta

Tudo a partir da Fase 2: análise de funções e jump tables, gerador de C++,
runtime (CPU, memória, syscalls), kernel do EE, GS/GIF/VIF/DMA, VU0/VU1,
IOP, SPU2, pad, memory card e CDVD. Os riscos de cada fase estão no
[PLANO.md](PLANO.md).

## Aspectos legais

O AnyPS2 não contém BIOS, firmware, chaves nem código da Sony. Todo o
comportamento do sistema é reimplementado em HLE. Use apenas dumps de jogos
que você possui. "PlayStation" e "PS2" são marcas da Sony Interactive
Entertainment; este projeto não tem relação com a Sony.
