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

> **Status: Fases 1 a 4 concluídas.** Homebrews do ps2dev — de console
> (printf, threads, timers, arquivos) e gráficos 2D/3D (libgraph/libdraw e
> gsKit) — são recompilados e rodam nativos, com o Graphics Synthesizer
> emulado em software e janela SDL2. Ainda não há VU (microprogramas), som,
> controle nem jogos. Veja o [PLANO.md](PLANO.md) para o roteiro completo.

| `gfx2d` (libgraph + libdraw) | `cube3d` (Z-buffer, textura em perspectiva) | `gskit` |
|---|---|---|
| ![gfx2d](tests/homebrew/gfx2d/expected.png) | ![cube3d](tests/homebrew/cube3d/expected.png) | ![gskit](tests/homebrew/gskit/expected.png) |

Essas imagens são a saída real dos homebrews recompilados (e as
referências que os testes comparam byte a byte).

## Experimente

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
build/tools/anyps2 recomp tests/homebrew/hello/hello.elf -o /tmp/hello
cmake -S /tmp/hello -B /tmp/hello/build && cmake --build /tmp/hello/build
/tmp/hello/build/hello a b
# Ola do PS2! argc=3
```

O `hello.elf` é um homebrew comum do ps2dev (`printf` do newlib). No
executável nativo, o `printf` percorre o mesmo caminho que no console: newlib
→ libcglue → `fioWrite` → SIF RPC → servidor fileio do IOP (aqui em HLE) →
stdout.

Com gráficos (abre uma janela; feche-a para sair):

```sh
build/tools/anyps2 recomp tests/homebrew/cube3d/cube3d.elf -o /tmp/cube3d
cmake -S /tmp/cube3d -B /tmp/cube3d/build && cmake --build /tmp/cube3d/build
/tmp/cube3d/build/cube3d
# ou sem janela, gravando a imagem final:
ANYPS2_VIDEO=none ANYPS2_SCREENSHOT=cubo.png /tmp/cube3d/build/cube3d
```

## O que funciona hoje

### Fase 4 — gráficos

- **DMAC**: canais VIF0/VIF1/GIF/SPR em modo normal e chain (todos os tags,
  `call/ret` com pilha, IRQ, TTE), D_STAT/D_PCR/D_ENABLE, interrupções por
  canal entregues aos handlers do programa, `BC0T/BC0F` ligados ao DMAC.
- **GIF**: PATH2/PATH3 (DMA e FIFO), GIFtag PACKED/REGLIST/IMAGE, A+D.
- **VIF0/VIF1**: todos os VIFcodes exceto execução de microprograma
  (`MSCAL`/`MSCNT`, Fase 5): `UNPACK` em todos os formatos com máscara,
  modos e escrita com salto, `MPG`, `DIRECT/DIRECTHL`, `STROW/STCOL`...
- **Graphics Synthesizer em software**: VRAM de 4 MB com o swizzle real de
  todos os formatos, todas as primitivas, Gouraud, texturas (32/24/16 bits,
  8/4 bits com CLUT, TEXA, wrap/clamp/região, bilinear, mipmaps, perspectiva),
  fog, testes de alfa/destino/Z, blending do GS, dither, máscaras, scissor,
  transferências HOST→LOCAL e LOCAL→LOCAL, SIGNAL/FINISH/LABEL e saída de
  vídeo com os dois circuitos (PMODE, DISPFB, DISPLAY, BGCOLOR).
- **Janela SDL2** (thread própria; o renderer do SDL usa OpenGL/Direct3D para
  apresentar), modo sem janela e screenshot em PNG.

O GS é emulado **em software**, como referência exata. Não há (ainda) um
renderizador do GS por GPU: ver a decisão e os números de desempenho no
[PLANO.md](PLANO.md#fase-4--gráficos-gifvifdma--graphics-synthesizer-).

### Fase 2 — recompilação e runtime

- **`anyps2 recomp`** gera um projeto CMake: uma função C++ por função MIPS,
  delay slots e branches likely, chamadas diretas, despacho dinâmico para
  `jr`/`jalr`, e reentrada no meio de funções (o que faz `setjmp/longjmp` e
  retornos por registrador funcionarem).
- **Runtime**: CPU com registradores de 128 bits, memória de 32 MB com
  espelhos e scratchpad, semântica de todas as instruções do EE exceto as
  vetoriais do VU0 (MIPS III, MMI completo, FPU do PS2 sem IEEE).
- **Kernel do EE em HLE**: threads com o escalonador do kernel real
  (prioridade estrita, troca só em syscalls/interrupções), semáforos,
  alarmes, handlers de interrupção DMAC/INTC, heap, argv, OSD, e as syscalls
  que o crt0 do ps2sdk usa.
- **Tempo e interrupções (Fase 3)**: relógio do EE real ou virtual
  (determinístico), timers T0–T3 com comparação/overflow, VBlank a 59,94 Hz
  (INTC, `GS_CSR`, `SetVSyncFlag`). O código gerado tem safepoints nos
  laços, então interrupções chegam mesmo durante espera ativa; o
  `DelayThread` do ps2sdk funciona sobre o Timer 2 emulado.
- **IOP em HLE via SIF**: os comandos SIFCMD/SIF RPC que o programa envia por
  DMA são interpretados e respondidos pelo protocolo real (o handler de
  interrupção do próprio programa roda). Servidores: `fileio` (`tty:` e
  `host:`, este relativo a `ANYPS2_HOST_DIR`) e `iopheap`.
- Tudo que falta lança erro claro: syscall não implementada (com nome),
  registrador de hardware desconhecido (com endereço), instrução do VU0,
  servidor RPC ausente, deadlock entre threads (com a lista de threads).

Variáveis de ambiente do executável gerado:

| Variável | Efeito |
|---|---|
| `ANYPS2_TRACE=syscall,iop,hw,gs,call` | imprime syscalls/trocas de thread, comandos SIF, acessos a hardware, DMA/GIF/VIF e chamadas |
| `ANYPS2_CLOCK=virtual` | relógio determinístico (padrão: `real`) |
| `ANYPS2_VIDEO=sdl\|none` | janela ou sem janela (padrão: janela se houver display) |
| `ANYPS2_SCREENSHOT=arquivo.png` | grava a última imagem exibida ao terminar |
| `ANYPS2_HOST_DIR=dir` | raiz do dispositivo `host:` (padrão: diretório atual) |
| `ANYPS2_IMAGE=arquivo` | imagem do programa (padrão: ao lado do executável) |

### Fase 1 — ELF e decodificador

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

Requisitos: CMake ≥ 3.20, um compilador C/C++20 (MSVC 2022, GCC ≥ 11 ou
Clang ≥ 14) e **SDL2** (Linux: `libsdl2-dev`; no Windows o CMake baixa e
compila o SDL2 automaticamente — `-DANYPS2_FETCH_SDL=ON`, padrão lá). Sem SDL:
`-DANYPS2_WITH_SDL=OFF` (só modo sem janela). O build do Windows/MSVC está no
CI mas não foi validado localmente.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Opções: `-DANYPS2_WARNINGS_AS_ERRORS=ON` (usado no CI),
`-DANYPS2_BUILD_TESTS=OFF`, `-DANYPS2_E2E_TESTS=OFF` (pula os testes de ponta
a ponta, que recompilam e compilam 8 homebrews, ~1 min com `ctest -j8`).

### Testes

| Suíte | O que verifica |
|---|---|
| `decoder`, `decoder_coverage`, `disasm`, `golden` | Fase 1: decodificador e disassembler (ver abaixo) |
| `elf`, `fixture` | parser de ELF, 20 mil mutações, fixture real |
| `runtime_ops` | semântica por instrução: aritmética, divisão por zero, overflow, shifts, loads parciais, MMI, FPU do PS2, mapa de memória, registradores de hardware |
| `codegen` | descoberta de funções, rótulos/entradas, C++ gerado, imagem |
| `timing` | relógio virtual/real, timers (prescaler, COMP, ZRET, overflow, flags), VBlank/`GS_CSR`, alarmes |
| `gs` | 21 casos: layout da VRAM por formato, cobertura de triângulos (sem pixel duplicado em aresta compartilhada), sprites, Gouraud, Z, blending, testes de alfa, CLUT, bilinear, perspectiva, saída de vídeo, GIF (PACKED/REGLIST/IMAGE), VIF (UNPACK, máscara, MPG, DIRECT), DMAC (chain, normal, SPR) |
| `e2e_hello`, `e2e_cputest`, `e2e_threads`, `e2e_fileio`, `e2e_timers`, `e2e_timers_real` | homebrews do ps2dev recompilados, compilados e executados; saída comparada (relógio virtual, e `timers` também no real) |
| `e2e_gfx2d`, `e2e_cube3d`, `e2e_gskit` | homebrews gráficos; saída de texto **e imagem final** (PNG) comparadas byte a byte com `expected.png` |

O `cputest` usa como oráculo o mesmo `main.c` compilado para o host
(inteiros de 32/64 bits, jump tables, ponteiros de função, recursão,
`setjmp/longjmp`, qsort, libc, ponto flutuante) e traz uma parte em assembly
do R5900 que se auto-verifica (delay slots, likely, `bal`, LWL/LWR/LDL/SDL,
38 operações MMI, FPU do PS2: divisão por zero → ±Fmax, saturação etc.).

Os ELFs dos homebrews são versionados; para regerá-los é preciso o toolchain
do ps2dev (Docker): `tests/homebrew/build.sh`.

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

`anyps2 recomp jogo.elf -o saida/ [--name NOME] [--function 0xENDERECO]`
gera o projeto e mostra um relatório (funções, instruções, instruções ainda
não suportadas que lançariam erro se executadas).

## Estrutura do repositório

```
common/       utilitários compartilhados (erros, leitura little-endian)
recompiler/   biblioteca: ELF, decodificador/disassembler R5900, análise de
              funções (analysis/) e gerador de C++ (codegen/)
runtime/      biblioteca linkada pelo código gerado: contexto, memória,
              semântica das instruções (ops.h), kernel do EE, hardware, IOP/SIF,
              DMAC, GIF, VIF, GS em software (gs/) e vídeo (SDL2)
cmake/        AnyPS2Runtime.cmake (incluído pelos projetos gerados)
tools/        CLI anyps2
tests/        framework mínimo, testes unitários, golden do objdump, fixtures,
              homebrews de ponta a ponta (homebrew/) e scripts
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

- Fase 5: VU0/VU1 (macroinstruções do VU0 e microprogramas via `MSCAL`
  hoje lançam erro; por isso a `libmath3d` e as amostras com VU1 ainda não
  rodam).
- Fase 6: módulos do IOP (pad, memory card, CDVD, SPU2/áudio, carregar IRX).
- Fase 7: jogos comerciais.

Limitações atuais (detalhes no [PLANO.md](PLANO.md)): a FPU usa o
arredondamento do host (o PS2 trunca), não há suporte a código carregado em
tempo de execução (overlays), o GS em software é mono-thread (~19 Mpixels/s
com textura bilinear — suficiente para homebrews, não para jogos
comerciais), DMA termina instantaneamente, e só há vídeo NTSC.

## Aspectos legais

O AnyPS2 não contém BIOS, firmware, chaves nem código da Sony. Todo o
comportamento do sistema é reimplementado em HLE. Use apenas dumps de jogos
que você possui. "PlayStation" e "PS2" são marcas da Sony Interactive
Entertainment; este projeto não tem relação com a Sony.
