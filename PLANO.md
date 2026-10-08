# PLANO — AnyPS2

Recompilação estática de executáveis do PlayStation 2 (EE / MIPS R5900) para
PC (Windows e Linux): **ELF do PS2 → C++ → binário nativo**, com um runtime
que implementa em HLE o kernel do EE, o IOP e o hardware gráfico/sonoro.

Regras do projeto (valem para todas as fases):

- C++20 + CMake + SDL; compila em MSVC, GCC e Clang (CI nos três).
- Nada de BIOS, firmware, chaves ou código da Sony no repositório. Tudo é
  HLE escrito por nós. Jogos só a partir de dumps do próprio usuário.
- Estado não suportado **nunca** passa em silêncio: erro claro com a
  instrução, o endereço, a syscall ou o registrador de hardware que faltou.
- Cada fase termina com testes automatizados passando e o README atualizado.
- Riscos e estimativas são registrados aqui com honestidade (seção
  "Riscos" de cada fase).

Legenda: ✅ feito · 🔜 próxima · ⬜ pendente

---

## Arquitetura

```
              ┌──────────────┐   ┌──────────────────────┐   ┌───────────────┐
ELF do PS2 ──▶│ recompiler/  │──▶│ projeto C++ + CMake  │──▶│ executável    │
              │  elf, r5900, │   │ (código gerado)      │   │ nativo        │
              │  análise,    │   └──────────┬───────────┘   └───────────────┘
              │  codegen     │              │ linka
              └──────────────┘   ┌──────────▼───────────┐
                                 │ runtime/             │  CPU (contexto), memória,
                                 │                      │  kernel EE (HLE), GS, VIF,
                                 │                      │  GIF, DMA, VU, IOP (HLE),
                                 │                      │  SPU2, pad, CDVD, SDL
                                 └──────────────────────┘
tools/anyps2  → CLI que roda o pipeline (info, disasm, recomp, ...)
tests/        → testes por instrução, oráculo diferencial, homebrew ponta a ponta
common/       → utilitários compartilhados (erros, bytes)
```

Referências estudadas (só como referência de arquitetura; nenhum código
copiado): **PS2Recomp** (analisador gera TOML → recompilador gera C++ → runtime
com tabela de despacho por endereço, stubs por nome/endereço, patches por
instrução), **N64Recomp** (uma função C++ por função MIPS, tabela de funções
para chamadas indiretas, overlays), **AnyPS5** (relinker + bibliotecas de
sistema nativas; estados inesperados lançam exceção).

---

## Fase 1 — Parser de ELF + decodificador R5900 ✅

| Tarefa | Status |
|---|---|
| Estrutura do repositório, CMake, CI (GCC, Clang, MSVC, ASan/UBSan) | ✅ |
| Parser ELF32 LE MIPS: cabeçalho, program headers, seções, símbolos, REL/RELA, numeração estendida | ✅ |
| Validação de limites com mensagens claras (offsets, sobreposição de PT_LOAD, strings sem terminador...) | ✅ |
| Visão de memória virtual (PT_LOAD, bss como zero), busca de símbolos por nome/endereço | ✅ |
| Reconhecimento de módulos IRX (ET_SCE_IOPRELEXEC) e de e_flags R5900 | ✅ |
| Tabela única de instruções (X-macro `opcodes.def`): 373 instruções com formato, flags e máscara de bits reservados | ✅ |
| Decodificador: MIPS III (sem as que o EE não tem), MMI (MMI0–3, PMFHL/PMTHL), COP0 (incl. debug/perf), COP1 do R5900, COP2/VU0 macro (Special1/Special2) | ✅ |
| Metadados para o gerador: desvio/salto/link/likely/delay slot, alvos, load/store com tamanho, trap/exceção, privilégio | ✅ |
| Detecção de codificação não canônica (bits reservados ligados) | ✅ |
| Disassembler na sintaxe do GNU objdump | ✅ |
| Testes: 1 codificação de referência por instrução + cobertura total do enum | ✅ |
| Oráculo diferencial contra GNU objdump `-m mips:5900` (golden versionado + verificação de ~1,6 M palavras no CI) | ✅ |
| Fixture ELF real (GNU as `-march=r5900`) + fuzz do parser | ✅ |
| CLI: `info`, `symbols`, `disasm`, `disasm-bin` | ✅ |

## Fase 2 — Gerador de C++ + runtime mínimo ✅

Meta: um homebrew de console (printf) compilado com o ps2dev rodando nativo.
**Atingida**: `tests/homebrew/hello` (ps2dev, GCC 15.2) imprime nativo; outros
três homebrews (CPU, threads, arquivos) rodam com saída idêntica à esperada.

### 2.1 Análise (recompiler/analysis)
- ✅ Descoberta de funções: símbolos `STT_FUNC`, entry point, alvos de `JAL`,
  lacunas de código sem símbolo (ignorando padding), `--function` na CLI.
- ✅ Rótulos internos, retornos de chamada e alvos de desvios externos viram
  pontos de entrada.
- ✅ Jump tables sem casar padrões: toda palavra de `.data/.rodata` (e todo par
  `lui/addiu|ori`) que aponta para código vira ponto de entrada, e cada
  `jr reg` faz um `switch` local sobre os rótulos da função, caindo no
  despacho dinâmico se o alvo for outra função.
- ⬜ Arquivo TOML por jogo (stubs, funções a pular, patches): adiado para a
  Fase 7, quando houver um jogo comercial que precise. Hoje há `--function`.

### 2.2 Gerador (recompiler/codegen)
- ✅ Uma função C++ por função MIPS (`fn_XXXXXXXX`), com `goto` para rótulos.
- ✅ Delay slots (condição/alvo lidos antes do slot), branches likely, slot
  que também é alvo de desvio, `bal` para obter o PC, `jalr rd` com rd ≠ ra.
- ✅ Chamadas diretas com verificação do endereço de retorno; reentrada no
  meio da função por um `switch` de entrada. Como todo o estado do guest vive
  no `Context`, o despachante pode retomar qualquer ponto de entrada —
  `setjmp/longjmp` e retornos por registrador funcionam.
- ✅ Instruções inválidas e macroinstruções do VU0 viram
  `unsupported(...)`, que lança erro com endereço e texto se executadas, e são
  contadas no relatório do `anyps2 recomp`.
- ✅ Projeto CMake gerado (`anyps2 recomp jogo.elf -o saida/`), código dividido
  em arquivos para compilar em paralelo, imagem dos segmentos ao lado do
  executável.

### 2.3 Runtime mínimo (runtime/)
- ✅ `Context` com GPRs de 128 bits (union portátil, sem `__int128`), HI/LO e
  HI1/LO1, SA, FPU, COP0, registradores do VU0 usados por QMFC2/QMTC2/LQC2/SQC2.
- ✅ Memória: 32 MB com espelhos (kuseg, kseg0/1, 0x2000_0000, 0x3000_0000),
  scratchpad, page table de 4 KB, MMIO por dispositivo; acesso desalinhado ou
  não mapeado lança erro com endereço e PC.
- ✅ Semântica de todas as instruções do EE fora do VU0: MIPS III do R5900,
  MMI completo, FPU do PS2 (sem NaN/Inf/denormais, saturação em ±Fmax, flags).
- ✅ Kernel do EE em HLE (≈90 syscalls nomeadas; as ausentes lançam erro com o
  nome): SetupThread/SetupHeap/argv, OSD, GS IMR, Copy/SetSyscall/TLB usados
  pelo crt0 do ps2sdk, handlers de INTC/DMAC.
- ✅ SIF em HLE no nível do protocolo (não por stubs de função): o IOP do HLE
  interpreta os comandos SIFCMD/SIF RPC enviados por `sceSifSetDma` e responde
  escrevendo no buffer do EE + interrupção DMAC SIF0, executando o handler do
  próprio programa. Servidores: fileio (tty:, host:) e iopheap.
- ✅ Registradores de hardware conhecidos (timers, INTC, DMAC, GS, SIO) e erro
  claro para os desconhecidos ou para iniciar DMA (Fase 4).

### 2.4 Testes
- ✅ 10 testes de semântica do runtime (vetores por instrução e casos de borda).
- ✅ 5 testes da análise/gerador sobre o fixture.
- ✅ 4 homebrews de ponta a ponta no CTest (recomp → cmake → build → run → diff):
  `hello`, `cputest` (oráculo: o mesmo C compilado no host + asm do R5900
  auto-verificado: delay slots, likely, loads parciais, 38 MMI, FPU do PS2),
  `threads` (ordem de escalonamento), `fileio` (host:).
- ✅ Código gerado + runtime limpos sob ASan/UBSan.

### Limitações conhecidas da Fase 2
- FPU: ~~o PS2 arredonda em direção a zero; usamos o arredondamento do host~~
  corrigido na Fase 5 — a FPU e os VUs truncam como o hardware (ver
  `ps2float.h`). Valores com expoente 255 (que no PS2 são números normais)
  continuam aproximados por ±Fmax.
- Código automodificável ou carregado em tempo de execução (overlays) não é
  suportado: só o que está no ELF é recompilado.
- Salto para um endereço que a análise não marcou como entrada gera erro
  claro; a correção é `--function 0x...` (ou, no futuro, o TOML).
- Desempenho ainda não foi trabalhado (estado sempre em memória).

## Fase 3 — Kernel do EE: threads, semáforos, timers, interrupções ✅

- ✅ Threads do EE (antecipadas na Fase 2 porque o crt0 do ps2sdk já cria uma):
  Create/Delete/Start/Exit/ExitDelete/Terminate, Sleep/Wakeup/CancelWakeup,
  Suspend/Resume, ChangeThreadPriority, RotateThreadReadyQueue, ReleaseWait,
  ReferThreadStatus e variantes `i*`. Cada thread do EE roda numa thread do
  host com pilha de 64 MB, mas só uma executa por vez ("bastão"), com
  prioridade estrita como no kernel real.
- ✅ Semáforos (Create/Delete/Signal/Wait/Poll/ReferSemaStatus + `i*`).
- ✅ Relógio do EE em ciclos, em dois modos: **real** (tempo do host; quando
  todas as threads dormem, o runtime dorme até o próximo evento) e
  **virtual** (`ANYPS2_CLOCK=virtual`: determinístico, o tempo avança com as
  instruções executadas e salta para o próximo evento quando todas dormem).
- ✅ Safepoints: o gerador desconta um orçamento de instruções em todo
  desvio para trás; ao acabar, o runtime avança o relógio, dispara eventos,
  entrega interrupções e pode trocar de thread. Laços de espera ativa sem
  syscalls (flag escrita por handler, polling de `GS_CSR`) funcionam.
- ✅ Timers T0–T3: contagem com prescaler (BUSCLK, /16, /256, HBLANK), COMP,
  ZRET, overflow, flags EQUF/OVFF "escreve 1 para limpar", interrupções INTC
  9–12. O sistema de timers do ps2sdk (base do `DelayThread`) roda em cima
  do T2 sem nenhum stub.
- ✅ VBlank sintético NTSC (~59,94 Hz): INTC VBLANK_S/VBLANK_E, `GS_CSR.VSINT`
  e `FIELD`, `SetVSyncFlag`.
- ✅ Alarmes do kernel (`SetAlarm`/`ReleaseAlarm`/`i*`, unidade HSYNC).
- ✅ Entrega de interrupções: INTC_STAT/INTC_MASK, handlers em contexto de
  interrupção (sem aninhamento), troca de thread ao fim da interrupção quando
  um handler acorda uma thread de prioridade maior.
- ✅ Deadlock: no relógio virtual, 60 s do EE sem nenhuma thread pronta vira
  erro listando as threads (no relógio real o programa pode legitimamente
  esperar para sempre, como no console).
- ➖ Event flags: o kernel do EE não os implementa de forma utilizável (o
  ps2sdk nem expõe a API); as syscalls lançam erro explícito.
- ✅ Testes: unitários do `Timing` (7 casos) e o homebrew `timers` de ponta a
  ponta nos dois relógios (DelayThread, alarmes, VBlank, laço sem syscall,
  preempção por interrupção).

Limitações: o tempo virtual conta ~1 ciclo por instrução (o EE real tem
stalls de cache/memória); temporização fina de jogos pode exigir calibração
por jogo na Fase 7. VBlank só em NTSC por enquanto (PAL quando houver
SetGsCrt com modo PAL relevante).

## Fase 4 — Gráficos: GIF/VIF/DMA + Graphics Synthesizer ✅

- ✅ DMAC: registradores dos 10 canais e globais (D_CTRL, D_STAT com CIS/CIM,
  D_PCR, D_ENABLER/W com suspensão). Modo normal e chain de origem (tags
  `refe/cnt/next/ref/refs/call/ret/end`, pilha ASR0/ASR1, IRQ+TIE, TTE nos
  VIFs) para VIF0, VIF1, GIF e toSPR; modo normal no fromSPR. A transferência
  roda inteira quando `CHCR.STR` é ligado; a interrupção do DMAC chega no
  próximo safepoint, despachada pelo kernel aos handlers de cada canal.
  `BC0T/BC0F` (CPCOND0 = canais de D_PCR terminados) — usado pelo
  `dma_wait_fast` do ps2sdk.
- ✅ GIF: PATH2 (VIF1) e PATH3 (DMA e GIF_FIFO), GIFtag com PRE/PRIM,
  PACKED (todos os descritores, Q do ST, ADC do XYZ), REGLIST (inclusive
  NREG×NLOOP ímpar), IMAGE, A+D. PATH1 (XGKICK) chegou com o VU1 na Fase 5.
- ✅ VIF0/VIF1: NOP, STCYCL, OFFSET, BASE, ITOP, STMOD, MSKPATH3, MARK,
  FLUSH*, STMASK, STROW, STCOL, MPG, DIRECT/DIRECTHL, UNPACK (S/V2/V3/V4 de
  32/16/8 bits, V4-5, sinal/USN, TOPS, máscara, modos offset/difference,
  escrita com salto CL ≥ WL). Memórias dos VUs mapeadas no EE
  (0x1100_0000). `MSCAL/MSCALF/MSCNT` chegaram na Fase 5.
- ✅ GS em software (referência): VRAM de 4 MB com o swizzle de todos os
  formatos (PSMCT32/24/16/16S, PSMT8/4/8H/4HL/4HH, PSMZ32/24/16/16S —
  validado contra as tabelas publicadas), registradores privilegiados
  (PMODE, SMODE, DISPFB/DISPLAY 1/2, BGCOLOR, CSR com SIGNAL/FINISH/HSINT/
  VSINT/FIELD, IMR, SIGLBLID) e gerais, fila de vértices (kick com XYZ2/F2,
  XYZ3/F3 sem desenho), pontos, linhas, line strips, triângulos (regra
  top-left em 12.4), strips, fans, sprites; Gouraud/flat; texturas com
  CLUT (CSM1/CSM2, CSA, CLD 0–5), TEXA, REPEAT/CLAMP/REGION, nearest/
  bilinear, mipmaps com LOD (MTBA → erro), STQ com perspectiva e UV;
  TFX (modulate/decal/highlight/highlight2), fog, teste de alfa com AFAIL,
  teste de alfa de destino, Z (32/24/16 bits), blending (A−B)·C/128+D com
  PABE/FBA/COLCLAMP, dither (DIMX), FBMSK, SCANMSK, scissor, XYOFFSET,
  PRMODECONT/PRMODE; transferências HOST→LOCAL (inclusive 24 e 4 bits) e
  LOCAL→LOCAL. Saída de vídeo: os dois circuitos de leitura com MAGH/MAGV,
  DBX/DBY, mistura por ALP ou alfa do pixel, fundo BGCOLOR.
- ✅ Apresentação: janela SDL2 (thread própria, renderer do SDL — OpenGL/
  Direct3D conforme a plataforma), proporção 4:3. Modo sem janela
  (`ANYPS2_VIDEO=none`) e screenshot PNG ao terminar (`ANYPS2_SCREENSHOT`),
  com compressão determinística (mesmos bytes em qualquer compilador).
- ✅ Testes: 21 testes unitários do GS/GIF/VIF/DMAC com valores calculados à
  mão (layout da VRAM, cobertura sem pixels duplicados em arestas
  compartilhadas, Gouraud, Z, blending, alfa, CLUT com índice trocado,
  bilinear, perspectiva, GIF em todos os modos, VIF, DMA chain/normal/SPR)
  — conferidos com testes de mutação; 3 homebrews de ponta a ponta com a
  imagem final comparada byte a byte: `gfx2d` (libgraph/libdraw/libdma:
  sprites, Gouraud, leque, linhas, pontos, translúcido, textura PSMT8+CLUT
  via DMA chain), `cube3d` (cubo 3D com Z-buffer, textura em perspectiva,
  bilinear, iluminação, double buffering) e `gskit` (a biblioteca gráfica
  mais usada em homebrews). As imagens são idênticas entre GCC e Clang.
- ✅ Corrigido nesta fase: corrida na criação da thread principal do guest
  (uma troca de thread muito rápida podia criar duas threads do host para a
  mesma thread do EE). Achado com ThreadSanitizer, que agora roda no CI
  junto com ASan/UBSan — inclusive sobre o código recompilado.

Decisão de backend: **a referência é o rasterizador em software**; a janela
usa o renderer do SDL (que por baixo usa OpenGL/Direct3D) só para apresentar
a imagem. Um renderizador do GS por hardware (OpenGL 4.x/Vulkan) **não foi
feito**: o GS tem semântica que não mapeia direto em GPU (VRAM com swizzle
compartilhada entre formatos, texturas lidas do próprio framebuffer,
blending além do fixo das APIs, CLUT), e fazê-lo certo é um projeto do
tamanho desta fase inteira. Ficou para quando o desempenho exigir.

Limitações e riscos:
- **Desempenho (risco alto para a Fase 7):** o rasterizador é escalar e
  mono-thread: ~63 Mpixels/s sem textura e ~19 Mpixels/s com textura
  bilinear + blending (≈15 ms por tela 640×448). Sobra para homebrews, mas
  jogos comerciais desenham várias camadas por quadro (o GS real passa de
  1 Gpixel/s). Antes da Fase 7 será preciso paralelizar/vetorizar o
  rasterizador (como o renderer em software do PCSX2) ou fazer o backend por
  hardware.
- Não emulado (com aviso ou erro explícito): antialiasing AA1 (aviso, desenha
  sem AA), transferência LOCAL→HOST e leitura dos FIFOs, MFIFO, modo
  interleave, IPU, escrita de preenchimento do VIF (CL < WL), interrupção/
  parada por bit `i` dos VIFcodes, espera de PATH3 mascarado, `TEX1.MTBA`.
- O DMA termina instantaneamente: programas que medem a duração de uma
  transferência ou dependem de PATH3 intercalado com PATH2 podem se
  comportar diferente.
- V3 no UNPACK preserva o W da memória (no hardware é "indeterminado");
  V2 replica x,y em z,w (como o PCSX2).
- Saída de vídeo: entrelaçado é mostrado como quadro completo (sem
  emulação de campo/deinterlace); HSINT é calculado sob demanda e não gera
  interrupção; só NTSC (o ROMVER sintético agora diz EUA/NTSC — antes dizia
  Europa, e a libgraph escolhia PAL).

## Fase 5 — VU0/VU1 e recompilação de microcódigo ✅

- ✅ Biblioteca `vu/`: decodificador e disassembler do microcódigo (pares
  upper/lower, bits I/E/M/D/T, todas as instruções do VU0/VU1 com EFU),
  validados contra o `dvp-objdump` do ps2dev em 12 mil pares aleatórios
  (`tests/data/vu_golden.txt`, mesma saída byte a byte, inclusive quais
  codificações o objdump rejeita).
- ✅ Aritmética do PS2 (`ps2float.h`), compartilhada pela FPU do EE e pelos
  VUs: sem NaN/Inf/denormais, saturação em ±Fmax com flags O/U e
  **arredondamento em direção a zero calculado exatamente** (TwoSum e
  produtos exatos em double — sem trocar o modo de arredondamento do host),
  conferido contra `fesetround(FE_TOWARDZERO)` em 200 mil operações.
- ✅ Núcleo do VU com modelo de pipeline: VF escrito fica pronto em 4 ciclos
  (ler antes trava), flags MAC/status/clip visíveis 4 ciclos depois da
  instrução, Q (DIV/SQRT 7, RSQRT 13) e P (EFU 11–54 ciclos) com WAITQ/WAITP,
  upper e lower em paralelo (o lower lê os valores antigos; o upper vence se
  ambos escrevem o mesmo registrador), LOI, bit E com delay slot, desvios
  com delay slot, BAL/JR/JALR, TPC.
- ✅ VU0 em modo macro: o recompilador do EE emite cada COP2 (`vadd`,
  `vmula`, `vdiv`, `vsqi`, `viadd`, `vclip`...) como chamada ao mesmo núcleo;
  `CFC2/CTC2` com a semântica dos registradores de controle (FBRST, CMSAR0/1,
  status só com bits fixos graváveis...), `VCALLMS/VCALLMSR` (VU0 em modo
  micro), `BC2x` (VU0 nunca ocupado: execução síncrona).
- ✅ VU1: `MSCAL/MSCALF/MSCNT` pelo VIF1 com double buffering (TOPS/TOP,
  BASE/OFFSET, ITOP), `CTC2 CMSAR1`, `XTOP/XITOP`, `XGKICK` (PATH1 para o
  GIF), VU0 lendo os registradores do VU1 em 0x4000.
- ✅ **Recompilação estática do microcódigo** (`anyps2 recomp`): o
  microcódigo é localizado no ELF — pacotes VIF `MPG` montados em tempo de
  compilação (endereço de carga conhecido) e blocos "crus" que o programa
  envia com um MPG montado em tempo de execução (como o ps2sdk faz) — e cada
  bloco vira C++: trechos de até 64 pares com um rótulo por par e
  fall-through entre pares consecutivos; cada par chama o núcleo do VU com
  as operações como parâmetros de template (`if constexpr`), então sobra só
  o código daquela operação. Os blocos não dependem do endereço de carga: o runtime casa
  o conteúdo da micro memória com os blocos (índice pelo par no pc), e cada
  par confere que a micro memória ainda contém a instrução compilada; se
  não contém (outro programa por cima, upload parcial), o interpretador
  continua dali — inclusive no meio de um delay slot — e o código
  recompilado é retomado no próximo desvio.
  Microcódigo que só existe em tempo de execução (lido do disco, gerado):
  `ANYPS2_VU_DUMP=dir` grava a micro memória dos programas interpretados e
  `anyps2 recomp --vu-dumps dir` os inclui na próxima geração.
  `ANYPS2_VU=interp` força o interpretador; `ANYPS2_VU=compiled` faz de
  qualquer par interpretado um erro (usado nos testes).
- ✅ CLI: `anyps2 vu <elf|dump> [--disasm]` lista/desmonta o microcódigo
  encontrado; `anyps2 vu-gen` gera só o C++ dele.
- ✅ GS: o evento FINISH agora chega depois do tempo estimado de trabalho do
  GS (registradores pelo GIF + pixels), no relógio do runtime. Antes ele era
  instantâneo e o sample `draw/teapot` do ps2sdk travava: `graph_wait_vsync`
  faz `CSR |= CSR & 8`, o que limpava um FINISH já presente, e
  `draw_wait_finish` esperava para sempre. No hardware o GS ainda está
  desenhando nesse momento.
- ✅ Testes:
  - `vu_isa`: golden do dvp-objdump; `vu`: 12 casos com valores calculados à
    mão (truncamento, flags e sticky, MAC/clip atrasados, stalls, latência de
    Q, upper/lower em paralelo, LOI, bit E, BAL/JR, memória, EFU, erros
    explícitos, XGKICK e double buffering do VIF1), conferidos com testes de
    mutação;
  - `vu_diff`: **teste diferencial** interpretador × código recompilado com
    16 microprogramas aleatórios (pares do golden + desvios para a frente,
    laços contados, JR absoluto, LOI) × 48 estados iniciais aleatórios
    (inclusive floats com expoente 0/255): registradores, flags, ACC, Q/P/I/R,
    memória, ciclos e TPC idênticos; também com o bloco carregado em outra
    base e com um par alterado na micro memória (volta ao interpretador e
    retoma). Conferido com mutação no gerador (desvios ignorados → 792
    diferenças). No build com sanitizers, o C++ gerado deste teste é
    compilado sem UBSan (com UBSan levava mais de 10 min; o mesmo núcleo
    continua sob UBSan pelo interpretador);
  - ponta a ponta com imagem comparada byte a byte: `vu0math` (libmath3d +
    VU0 macro em assembly + microprograma via VCALLMS), `vu1draw` (cubo
    transformado por um microprograma do VU1, com XGKICK; roda também com
    `ANYPS2_VU=interp` e `ANYPS2_VU=compiled`, mesma imagem) e quatro samples
    do ps2sdk sem modificação: `draw/cube`, `draw/teapot`, `draw/texture` e
    `vu1/` (`ANYPS2_FRAMES=N` encerra no N-ésimo VBlank).

Desempenho medido (Release, GCC 13, 4 núcleos) — **o ganho da recompilação
do microcódigo é pequeno, e isso foi medido, não suposto**:
- interpretador ~35 Mpares/s, código recompilado ~37 Mpares/s (**~1,1x**);
- o custo por par está na semântica, igual nos dois caminhos: truncamento
  exato, flags MAC/status/clip (toda instrução FMAC produz flags que ficam
  prontas 4 ciclos depois) e o modelo de pipeline — não na decodificação,
  que o interpretador já guarda em cache;
- variantes testadas: tudo inline numa função por bloco deu 1,9x, mas levou
  5m44s para compilar 2 mil pares (crescimento não linear no GCC); uma função
  por par com o pipeline inline, 1,1x e 3 min. A versão final (trechos de 64
  pares, pipeline fora de linha, `-O1`) compila ~18 ms por par, linear e em
  paralelo por arquivo (um microprograma de 16 KB ≈ 40 s de CPU), com o
  mesmo desempenho de `-O3`;
- nos samples o efeito total é nulo: no `vu1/`, o VU inteiro é ~3% do tempo
  e o GS em software desenhando o que o XGKICK manda é ~22%.

O caminho para ganho real fica registrado para antes da Fase 7 (o VU1 de um
jogo pode precisar de 50–100 Mpares/s): análise estática no recompilador —
calcular os stalls e a latência das flags em tempo de compilação dentro de
cada bloco e eliminar flags que nunca são lidas (o sticky do status complica
isso: ele depende de todo resultado) — e avaliar as flags de forma
preguiçosa no runtime.

Limitações (honestas):
- O VU roda **síncrono**: o microprograma executa inteiro no MSCAL/VCALLMS e
  o EE não gasta tempo esperando; jogos que fazem trabalho no EE em paralelo
  ao VU1 e medem/esperam por isso de formas exóticas podem divergir.
- EFU (ESIN, EATAN, EEXP, ERSQRT...) usa a libm do host: o último bit pode
  diferir do hardware (os algoritmos exatos do EFU não são públicos).
- Pipeline: modelados os stalls de VF e de Q/P e a latência das flags;
  não modelados: stall de VI após load inteiro, a peculiaridade de desvio
  condicional logo após uma escrita de VI ("branch delay" do VI), escritas
  simultâneas de flags por FMAC e FDIV no mesmo ciclo, e bits D/T (erro).
- Microcódigo que não está no ELF nem nos dumps roda no interpretador
  (correto, só mais lento).

## Fase 6 — IOP em HLE ✅

O IOP não executa código: cada comando SIF que o EE manda é interpretado na
hora e os módulos (IRX) são reimplementados em C++ por protocolo RPC
(`runtime/src/iop/`). Tudo escrito a partir dos protocolos públicos do
ps2sdk; nenhum código ou firmware da Sony.

- ✅ **Carregamento de módulos** (servidor `loadfile`): `SifLoadModule`
  (`rom0:`/`host:`/`cdrom0:`), `SifExecModuleBuffer` (IRX embutido no ELF e
  copiado por DMA para a RAM do IOP), `SifSearchModuleByName`,
  `SifIopGetVal/SifIopSetVal`. O módulo é identificado pelo nome gravado
  no IRX (`.iopmod`, ou a `ModuleInfo` dos IRX do ps2sdk) ou pelo nome do
  arquivo em `rom0:`, e "carregar" registra os servidores RPC da
  implementação HLE.
  Módulo sem HLE (ex.: `usbd.irx`, um driver próprio do jogo,
  `rom0:FOOBAR`) é erro explícito com nome, versão e origem. `SifIopReset`
  descarrega o que o programa carregou. `cdvdman/cdvdfsv`, `fileio`,
  `iopheap` e `loadfile` existem desde o boot, como no console.
- ✅ **Controles** (`padman`, os dois protocolos do libpad: XPADMAN/
  padman.irx `0x80000100` e PADMAN da ROM `0x8000010F`): DualShock 2
  emulado — modos digital (0x41), analógico (0x73) e com pressão (0x79),
  `padSetMainMode`, `padInfoMode`, `padEnterPressMode`, atuadores aceitos
  (sem vibração no host). A cada VBlank a estrutura de estado é escrita na
  área do EE (buffer duplo, como o módulo real). Entrada: teclado e
  controles (SDL_GameController) pela thread da janela, ou um **roteiro
  determinístico** (`ANYPS2_PAD_SCRIPT`) para testes. Sem multitap.
- ✅ **Memory card** (`mcman/mcserv`, libmc nas numerações nova e antiga):
  cada cartão é uma pasta do host (`$ANYPS2_MC_DIR/mc0`, `mc1`; padrão
  `$ANYPS2_HOST_DIR/memcard`). getInfo (com "cartão trocado" na primeira
  vez), open/close/read/write/seek/flush, mkdir, chdir, getDir com curinga,
  rename (setFileInfo), delete, format/unformat. O espaço livre segue o
  cartão de 8 MB (clusters de 1 KB). O FAT do cartão não é emulado em nível
  de bloco: não é possível usar um `.ps2` de outro emulador (nem
  `mcReadPage/mcWritePage`, que dão erro).
- ✅ **Disco** (`cdvdman/cdvdfsv`): imagem ISO própria em `ANYPS2_ISO`
  (2048 bytes/setor ou bruta 2352, modo 1/modo 2), ISO 9660 implementado do
  zero (volume primário, diretórios multi-setor). `sceCdInit`,
  `sceCdSearchFile`, `sceCdRead` (no EE e na RAM do IOP), `sceCdSync`,
  `sceCdDiskReady`, `sceCdGetDiskType` (CD/DVD pelo tamanho),
  `sceCdReadClock` (no relógio virtual, data fixa + tempo emulado), status,
  bandeja, seek/pause/stop; `cdrom0:` no fileio (open/read/lseek/getstat/
  dopen/dread; escrita recusada). Leituras são instantâneas.
- ✅ **SPU2 em software**: 2 MB de RAM, 48 vozes ADPCM (blocos com flags de
  loop), envelope ADSR (ataque/decay/sustain/release lineares e
  exponenciais), pitch, volume por voz, mixagem estéreo a 48 kHz.
  O mixer roda **no tempo emulado** (a cada VBlank gera as amostras do
  intervalo): com o relógio virtual, o som é idêntico em toda execução.
- ✅ **audsrv** (ps2sdk): PCM 8/16 bits mono/estéreo de 11025 a 48000 Hz no
  buffer circular do tamanho do original, convertido para 48 kHz como o
  módulo real; `audsrv_wait_audio` bloqueia o EE (resposta RPC adiada até
  haver espaço); volume; samples ADPCM (load/play/volume e pan/is_playing).
  `freesd`/`libsd` são aceitos (a API deles é para outros módulos do IOP).
- ✅ **Saída de som**: SDL2 (`ANYPS2_AUDIO=sdl`, ou automático se houver
  dispositivo) e/ou WAV (`ANYPS2_AUDIO_WAV=arquivo.wav`).
- ✅ Correções do SIF achadas aqui: resposta de RPC `NOWAIT` sem callback
  (`rmode` 0) agora também zera o pacote do cliente no EE, como o
  `sceSifExecRequest` do IOP — sem isso `sceSifCheckStatRpc` nunca dizia
  "terminado" (libmc travava); `dopen` e `mkdir` do fileio liam o nome no
  offset errado (bug da Fase 2, nunca exercitado).
- ✅ Build: o GCC 13 em Release (`-O3`) acusava falsos "null dereference"
  dentro da libstdc++ com `-Werror` — o job GCC do CI não compilava.
  `-Wnull-dereference` agora fica só no Clang (o `analyze()` ganhou também
  uma guarda defensiva para início de função fora de código).
- ✅ Testes:
  - unitários (`iop`): cabeçalho de IRX (nome no `.iopmod` e na
    `ModuleInfo`, rejeição de lixo), roteiro do pad (sintaxe, erros, troca
    por VBlank, desconexão), ISO 9660 (busca, leitura que cruza setores,
    listagem, imagem inválida), decodificação ADPCM (filtros, saturação),
    voz do SPU2 (fim do sample, loop, key off com release, volume por canal);
  - ponta a ponta: `modules` (rom0: + `padman.irx` embutido; `usbd.irx` e
    `rom0:FOOBAR` têm de parar com o erro claro), `pad` e `pad_rom` (mesmo
    roteiro, mesma saída nos dois protocolos do padman), `memcard`, `cdvd`
    (com `disc.iso` gerado por `make_iso.py`) e `cdvd_noiso` (erro claro),
    `audio` (audsrv.irx e freesd.irx do ps2sdk embutidos; o WAV gerado é
    comparado byte a byte — conferido à parte: 441 Hz por 0,3 s, depois a
    quadrada ADPCM de 428,6 Hz por 46,7 ms com o pan esperado).

Limitações (honestas):
- **Drivers IRX próprios** (a maioria dos jogos comerciais traz o seu driver
  de som e às vezes de streaming/controle) não rodam: o IOP não executa
  código. Para eles será preciso emular ou recompilar o próprio IOP (R3000A)
  — subprojeto de porte parecido com a Fase 2. Esse é o principal risco da
  Fase 7, e só um dump real vai dizer o quanto ele pesa.
- Sem `sdrdrv`/libsdr (acesso remoto ao libsd pelo EE): o bind falha com
  erro claro. Sem reverb, volumes em modo "sweep", ruído, entrada de
  disco/PCM no núcleo, CD-DA e o callback `audsrv_on_fillbuf` (exigiria o
  IOP chamar um servidor RPC do EE). A interpolação entre amostras é cúbica
  (Catmull-Rom), não a tabela gaussiana do chip.
- Leituras de disco instantâneas, sem tempo de busca; jogos que dependem
  do tempo de leitura para sincronizar podem se comportar diferente.
- `mc0:` pelo fileio (fopen) não é suportado — só pelo libmc.
- Multitap, mouse/teclado USB, rede e HDD não existem (erro claro no bind).

## Fase 7 — Primeiro jogo comercial 🔜

- ✅ Build MSVC validado localmente (VS Community 2026, MSVC 14.50): compila
  e todos os testes passam. Corrigido: o teste `codegen.project_files_and_image`
  apagava a pasta com arquivos ainda abertos (o Windows não deixa).
- ✅ `anyps2 disc <imagem.iso>`: triagem do disco (SYSTEM.CNF, executável
  principal, strings `rom0:`/`cdrom0:`/`host:`, IRX embutidos e do disco com
  nome/versão/HLE, imagens IOPRP, outros ELFs, maiores arquivos). A tabela de
  módulos com HLE, o ISO 9660 e o cabeçalho IRX viraram uma biblioteca
  pequena (`anyps2_iopfmt`) que a CLI usa sem puxar o runtime inteiro.
- ✅ ISO 9660: DVDs de camada dupla (DVD-9) — o volume da camada 1 é achado e
  seus arquivos têm LSN absoluto. (O `sceCdLayerSearchFile` do libcdvd, que
  busca na camada 1, ainda não existe no HLE.)
### Gran Turismo 4 (PAL, SCES-51719) — primeiro alvo

Triagem: DVD-9; o `SCES_517.19` (273 KB) é só o boot — o jogo de verdade é o
`CORE.GT4` (deflate → 6,1 MB de código cru do EE, carregado em tempo de
execução); 24 dos 28 IRX do disco sem HLE, inclusive drivers próprios da
Polyphony (`libpdi`, `pdicdvd`, `pdispu2`, `pdistr`, `rt_ac` embutido).

Marco 1 — o boot recompilado roda até a tela de copyright ✅ (desenhada pelo
GS em software) e para no primeiro módulo sem HLE (`mtapman`). Para chegar lá:
- ✅ **Tabela de syscalls visível ao guest** na RAM do kernel (0x80000800, 512
  entradas com sentinelas): `SetSyscall`/`GetEntryAddress` a usam, a
  `FindAddress` que a libkernel da Sony instala acha a base dela, e o despacho
  respeita o que o programa instalou — redirecionamento para outra syscall
  do HLE, handler do programa para syscalls que o HLE não implementa, e
  chamada direta a um sentinela. Patches de kernel que substituem syscalls
  que o HLE já implementa (alarmes, TLB) são absorvidos: o HLE continua.
- ✅ **ERET** com a semântica real (ERL → ErrorEPC, senão EPC; limpa o bit),
  gerado como salto indireto: a libkernel escreve nos timers do kernel em
  modo ERL e volta com `eret`.
- ✅ Bug corrigido: o retorno das syscalls ia para `v0` estendido com zeros; no
  EE valores de 32 bits ficam com extensão de sinal (comparar o retorno com
  um endereço de kernel lido da memória falhava).
- ✅ **fileio do SDK 3.0** (FILEIO_service 2.x do IOPRP300), engenharia reversa
  do lado do EE: função 255 registra dois buffers de conclusão; cada pedido
  leva {sema, endereço/tamanho do resultado}; o IOP preenche o buffer
  {sema, função, destino, tamanho, resultado, extras (dirent/stat no formato
  `iox_*`)} e manda o comando SIF 0x80000011, cujo handler do EE copia e faz
  `iSignalSema`. Implementados open/close/read/write/lseek/remove/rmdir/
  dopen/dclose/dread/getstat; os demais dão erro com nome e número. As
  operações são compartilhadas com o protocolo do ps2sdk.
- ✅ Versão `"3000"` nas funções 255 do fileio e do loadfile (a libsifdev da
  Sony recusa carregar módulos se não bater).
- ✅ **Reboot do IOP leva tempo**: as flags SIFINIT/CMDINIT/BOOTEND voltam na
  primeira leitura do SMFLAG depois do reset (o EE da Sony as limpa logo
  depois de mandar o reset e esperava para sempre).
- ✅ `rom0:ROMVER` (sintético) também pelo fileio; dados que o IOP manda junto
  com um comando SIF agora chegam ao EE na entrega do comando, como o DMA.
- ✅ Teste `e2e_kpatch` (homebrew novo): tabela de syscalls, redirecionamento,
  ERET nos dois modos, fileio do SDK 3.0 (cliente escrito a partir do
  protocolo, sem código da Sony), reboot na ordem da Sony, versões e ROMVER.

Marco 2 — o boot termina e o executável principal entra no projeto ✅:
- ✅ O boot carrega os 5 módulos dele (`sio2man`, `mtapman`, `mcman`,
  `mcserv`, `padman`), inicializa a libmc, lê o `CORE.GT4`, descomprime e
  chama `ExecPS2(0x00100008, …, ["cdrom0:\CORE.GT4;1", "hot"])`.
  - `mtapman` aceito sem servidores (sem multitap conectado; o RPC dele dá o
    erro de servidor inexistente se alguém o usar).
  - `Deci2Call` (depurador do kit) falha como num console de varejo;
    `RemoveSbusIntcHandler` sem handler instalado é no-op.
  - mcman declara a versão 0x20E (a libmc do SDK 3.0 exige ≥ 0x20E).
- ✅ **ExecPS2** no kernel: o programa atual termina (todas as threads), o
  estado do kernel do EE é descartado (threads, semáforos, handlers,
  alarmes) e o novo programa começa na entrada com gp e argv; memória, IOP
  e tabela de syscalls ficam. Se a entrada não é código recompilado: erro
  claro, e com `ANYPS2_EXEC_DUMP=pasta` a RAM inteira é gravada.
- ✅ **`anyps2 ram2elf`**: ELF sintético (segmentos + seções `.text`/
  `.data`/`.bss`) a partir da RAM gravada, com várias faixas de código e de
  dados. **`anyps2 recomp --extra X.elf`**: código que só existe em tempo de
  execução entra no mesmo projeto (tabela de funções única, checagem de
  sobreposição), fora da imagem inicial.
- ✅ O núcleo do GT4: `CORE.GT4` = 256 bytes (assinatura?) + cabeçalho
  {entrada, endereço de carga, tamanhos} + imagem copiada para
  0x00100000; texto 0x100000–0x616F20, dados até 0x6D5D98, bss até
  0x6DDDF0, e uma ilha de código SHA-1 (MMI) em 0x6C5400–0x6C8340 no meio
  dos dados. Recompilado com o boot: 15.629 funções, 1,38 M instruções,
  nenhuma instrução sem suporte.
- ✅ Testes: `e2e_execps2` (boot em 0x01000000 que copia um ELF embutido para
  a memória e chama ExecPS2; recompilado com `--extra`; o filho recebe argv
  e encontra o kernel zerado), `e2e_execps2_missing` (sem `--extra`: erro
  claro) e `cli_ram2elf`. `tests/homebrew/build.sh` aceita `LOADADDR`.

Marco 3 — o núcleo roda até os drivers da Polyphony ✅:
- ✅ **Controles do SDK 3.0** (`dbcman` + `sio2d` + `ds2u_d`, usados pela
  libdbc/libpad2): protocolo levantado do lado do EE — RPC 0x80001300
  (versão 0x0316, área de trabalho, criar/apagar/iniciar socket, consulta
  ao dispositivo), 0x8000131E/1F (vibração, aceita). Cada socket tem dois
  quadros de 128 bytes no EE que o IOP reescreve a cada VBlank (estado,
  tamanhos, status, os 18 bytes do DualShock 2, perfil "16 digitais + 16
  analógicos", contador). A entrada é a mesma do padman (teclado, controle,
  roteiro); o relatório do DS2 virou `ds2Report`, comum aos dois.
- ✅ `sceMcGetSlotMax` (mcserv 0x15): 1 slot (sem multitap).
- ✅ `devctl("dev9x:")` (e hdd/pfs) pelo fileio do SDK 3.0: -ENODEV, como num
  console sem adaptador de rede/HDD; os demais devctl dão erro explícito.
- ✅ Buffers dos servidores RPC do HLE: 16 KB (eram 64 KB, cabiam só 15
  servidores); pedido maior que o buffer é erro claro.
- ✅ Testes: `e2e_dbcpad` (fala o protocolo do dbcman com um IRX mínimo
  montado pelo teste — só o nome no cabeçalho; roteiro de controle com
  CROSS+START e porta 1 desconectada) e `e2e_kpatch` com o devctl.

Onde o GT4 está agora (com `ANYPS2_IOP_ACCEPT_MISSING=1` para os módulos de
rede/USB): o núcleo carrega `libsd`, `usbd`, a pilha de rede, `dev9`,
`msifrpc`, `PDI_Library` e `PDI_CDVD_Manager` e espera o RPC 0x50434456
("PCDV") do `pdicdvd`, o driver de disco da Polyphony — o muro previsto na
triagem.

Próximos passos do GT4: o HLE dos drivers da Polyphony por engenharia
reversa do lado do EE (pdicdvd primeiro: leitura do GT4.VOL/GT4L1.VOL;
depois pdispu2/pdistr/rt_ac, que são som e streaming — o caminho mais curto
é aceitar os comandos e o jogo rodar sem som; som de verdade exige executar
o código do IOP), HLE de "nenhum dispositivo" para rede/USB/EyeToy/volante/
impressora, e PAL.

- ⬜ A partir de um dump do usuário: análise do ELF principal e de overlays
  (muitos jogos carregam código extra do disco), configuração TOML por jogo.
- ⬜ Lista de compatibilidade (`docs/COMPATIBILIDADE.md`) com status por jogo.
- ⬜ Correções jogo a jogo, sempre com teste de regressão.

Riscos (alto): mesmo um jogo "simples" exercita quase todo o hardware. É
realista esperar que o primeiro jogo comercial jogável leve muito mais tempo
que as fases anteriores somadas; overlays de código carregados do disco
exigem recompilar também esses binários (a partir do mesmo dump).
