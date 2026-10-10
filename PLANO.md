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
  escrita com salto CL ≥ WL e de preenchimento CL < WL). Memórias dos VUs mapeadas no EE
  (0x1100_0000). `MSCAL/MSCALF/MSCNT` chegaram na Fase 5.
- 🟡 IPU (Fase 7): registradores CMD/CTRL/BP/TOP, FIFO de entrada com o
  buffer interno de 2 quadwords (BP/FP/IFC como no hardware), CTRL.RST e os
  comandos BCLR, FDEC, SETIQ, SETVQ e SETTH; comando sem dados suficientes
  fica ocupado e continua quando o FIFO recebe mais. DMA toIPU (normal e
  chain) sob demanda: enche o FIFO e pausa com STR ligado; cada quadword
  consumido puxa mais. VDEC (as 4 tabelas) e BDEC (macrobloco em RAW16)
  reiniciáveis; IDEC (fatia intra até RGB32) e CSC (RGB32), reiniciáveis
  por macrobloco; FIFO de saída e DMA fromIPU. Faltam PACK, a saída RGB16
  (IDEC/CSC com OFM) e o MPEG-1 no IDEC/BDEC.
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
  sem AA), leitura dos FIFOs do GIF/VIF0, MFIFO, modo
  interleave, PACK e saída RGB16 do IPU, espera de PATH3 mascarado,
  `TEX1.MTBA`.
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

Marco 4 — drivers da Polyphony e o jogo lendo o GT4.VOL ✅ (experimento,
sem som):
- ✅ **pdicdvd** (`PDI_CDVD_Manager`): RPC "PCDV" — pronto, conferência do
  PVD, leitura de setores para a memória do EE e o início da camada 1 do
  DVD-9; RPC "Pcdv" — buffer de status no EE com o relógio (segundos desde
  1999-12-30, fuso do Japão) reescrito a cada VBlank.
- ✅ **pdistr** (`PDI_Streaming_service`): RPC "STRP" — abrir um trecho do
  disco por LSN absoluto (handle ≠ 0), ler em pedaços direto na memória do
  EE, fechar. É assim que o jogo lê o GT4.VOL (3 GB, atravessa as duas
  camadas). Abrir por caminho e as funções 1/5/6/8/9 ainda não foram vistas
  em uso e dão erro claro.
- ✅ **lgdev** (volante Logitech, RPC 0x046D046D): versão 0x010B2400 (o
  jogo trava de propósito com outra) e enumeração sem volante (código
  negativo: o jogo para de procurar).
- ✅ **msifrpc** (SIF RPC multi-thread da Sony, libmrpc no EE, usado pela
  Libnet): handshake pelo SREG 1 e bind/call com o mesmo layout de pacote
  do SIF RPC, nos comandos 0x80000019/0x8000001A, respostas no 0x80000018.
- ✅ Exploração (`ANYPS2_IOP_ACCEPT_MISSING=1`): servidor RPC sem HLE vira um
  servidor que responde zeros e registra cada chamada (função, começo dos
  dados, strings) — foi assim que os protocolos acima foram levantados.
- ✅ `ANYPS2_PROFILE=1`: amostra o PC nos safepoints e imprime os mais
  frequentes ao terminar (achou a espera do msifrpc e a trava proposital
  do lgdev).
- ✅ Recompilador: a varredura de constantes (lui + addiu) olha 256 bytes
  adiante (o GT4 separa os pares por até 11 instruções); o gerador não
  regrava arquivos iguais e o código gerado não inclui mais `runtime.h`
  (mudar o runtime não recompila o jogo inteiro — 23 min no GT4).
- ✅ Teste `e2e_pdi`: fala os quatro protocolos (escritos a partir do que o
  jogo faz) com IRX mínimos só com o nome, sobre o `disc.iso` do teste cdvd.

Marco 5 — a primeira tela do jogo: seleção de idioma ✅ (sem som):
- ✅ **Parada do VIF pelo bit I**: o GT4 sincroniza o desenho com um sistema
  próprio — o VIF1 para no VIFcode com bit I até o handler da interrupção
  (causa 5) decidir, por contadores no scratchpad, se libera (FBRST.STC);
  tags de DMA com IRQ e o FINISH do GS (causa 0) mexem nos mesmos
  contadores. Agora o VIF liga STAT.VIS/INT ao fim do comando, pede a
  interrupção do INTC (4/5) e para; o resto do quadword fica no FIFO
  (STAT.FQC) e o DMA do canal pausa no ponto exato (MADR/QWC/TADR, STR
  ligado, também no meio do DMAtag com TTE). FBRST.STC processa o FIFO e o
  DMA continua; ERR.MII ignora o bit I.
- ✅ **Handlers com Status.IE desligado**: o GT4 escolhe entre `SignalSema` e
  `iSignalSema` olhando esse bit; com ele ligado, o `SignalSema` dentro do
  handler de VBLANK trocava de thread no meio da interrupção.
- ✅ **Vídeo entrelaçado em modo campo** (SMODE2 INT+FFMD): o framebuffer
  tem meia altura e cada linha aparece duas vezes na saída (o GT4 PAL usa
  640×256 por campo, PSMCT24, double buffering).
- ✅ pdistr fn 5: ler do stream para os dados de som (no marco 8 se viu que o
  destino é a RAM do SPU2, não a do IOP).
- ✅ Relógio real: dois FINISH do GS que vencem juntos (o GS em software
  gasta tempo de verdade) saem um por vez — o sample `draw/teapot` do
  ps2sdk travava em tela preta com a janela aberta.
- ✅ Testes: `gs.vif1_interrupt_bit_stalls_and_resumes_dma` (chain com
  FLUSH+I no meio dos dados e no DMAtag, modo normal parando no meio do
  quadword, ERR.MII), `gs.finish_real_clock_one_at_a_time` e o modo campo
  em `gs.display_output`.

Marco 6 — dos menus iniciais ao vídeo de abertura ✅ (sem som):
- ✅ **UNPACK com escrita de preenchimento** (STCYCL CL < WL): NUM conta os
  vetores gravados, só os CL primeiros de cada ciclo de WL são lidos do
  stream e os WL vão para quadwords seguidos; os demais vêm da máscara
  (ROW/COL). O GT4 usa CL=0 WL=4 com máscara toda ROW (nenhum dado lido);
  antes o VIF consumia 16 palavras e lia um float como VIFcode. Os valores
  de CL/WL são comparados crus: CYCLE=0 (nunca escrito) é escrita normal.
- ✅ VIFcode inválido mostra os últimos comandos aceitos (diagnóstico).
- ✅ D_ENABLEW (0x1000F590) lido de volta, como o D_ENABLER.
- ✅ `ANYPS2_SCREENSHOT_EVERY=N`: capturas periódicas para acompanhar um
  roteiro de controle; `jogos/run_gt4.sh` aceita `MC_DIR` (cartão limpo).
- ✅ Com um roteiro de controle o GT4 passa pela seleção de idioma, cria os
  dados no memory card, recebe o nome na tela "Welcome to GT World", grava
  o save e mostra a introdução; ao escolher "Start" para no **IPU**
  (`sceMpegInit`: SETIQ pelo FIFO de entrada) — o vídeo de abertura.

Marco 7 — o vídeo de abertura (em andamento):
- ✅ IPU, etapa 1: registradores, FIFO de entrada e ponteiro de bits, BCLR,
  FDEC, SETIQ, SETVQ e SETTH (testes em `ipu`). O `sceMpegInit` do GT4
  passa (matrizes de quantização pelo FIFO).
- ✅ **Servidores MPG1/MPG2 em HLE** (leitor de vídeos do PDISTR.IRX,
  levantado do código do IRX): fn 1 abre um MPEG-2 Program Stream no disco
  ({flags, LSN, setores, nome}; o vídeo de abertura fica no LSN 0x239CE4,
  fora do sistema de arquivos, na área da segunda camada), fn 2 enche
  fatias de 5120 bytes, uma por pacote de vídeo (0xE0), em registros
  {bytes, desalinhamento, bytes que faltam, 0} + dados + registro zerado; o
  EE remonta os pacotes PES. O áudio (0xBD) seria do SPU2 e é pulado (som
  mudo). Testes `iop.movie_program_stream_video_packets` e
  `iop.movie_slot_layout`.
- ✅ IPU, etapa 2: **DMA toIPU sob demanda** (normal e chain), como no
  hardware: o canal enche o FIFO de 8 quadwords e pausa com STR ligado;
  cada quadword que o IPU tira do FIFO puxa mais um, então MADR/QWC/TADR +
  IFC/FP/BP mostram exatamente quanto foi consumido. Reescrever o CHCR com
  o canal pausado reavalia o tag corrente (a libmpeg troca refe por ref ao
  acrescentar tags ao anel). Testes `ipu.dma_to_ipu_normal_on_demand` e
  `ipu.dma_to_ipu_chain_and_append`.
- Onde o GT4 está: o vídeo chega ao IPU pelo DMA, a libmpeg lê os
  cabeçalhos (sequence/GOP/picture) com FDEC e pede o primeiro **VDEC**
  (tipo de macrobloco): o caminho é VDEC + BDEC por macrobloco, com a
  compensação de movimento em software no EE.
- ✅ IPU, etapa 3: **VDEC e BDEC**. Tabelas de comprimento variável do
  anexo B escritas da norma, como os códigos aparecem nela, e conferidas
  entrada por entrada contra outra implementação (todas batem); consulta
  direta por tabela. VDEC devolve valor | (bits << 16) com as
  particularidades do IPU (escape/stuffing do MBAI = 0x23/0x22, tipo de
  macrobloco com frame_motion_type = quadro, sinal do motion_code
  engolindo o comprimento). BDEC: DC com predição (DCR), coeficientes
  pelas tabelas zero/um, escape de 12 bits, quantização inversa como o IPU
  (matriz indexada pela posição na varredura, saturação em ±2048, sem
  mismatch control), varredura zigue-zague ou alternada, DCT de quadro ou
  de campo, IDCT de referência em precisão dupla; saída RAW16 (intra
  0–255, não intra o resíduo); depois do macrobloco, start code à frente
  liga SCD. Comandos reiniciáveis: sem dados no meio, voltam ao começo e
  devolvem os quadwords ao FIFO. Saída numa fila (OFC até 8; o comando
  só termina quando o resto cabe no FIFO) e **DMA fromIPU** (modo normal,
  pausa até haver saída). Testes `ipu.mpeg_*`, `ipu.vdec_tables_and_top`
  e `ipu.bdec_*`.
- ✅ IPU, etapa 4: **CSC** (RAW8 YCbCr 4:2:0 → RGB32) com os coeficientes
  em ponto fixo do IPU (BT.601 em 1/64), alfa 0x80 e os limiares do SETTH
  (abaixo de TH0, pixel zerado; abaixo de TH1, alfa 0x40); reiniciável por
  macrobloco (o que já saiu não se refaz). Testes `ipu.csc_*`.
- ✅ Comando reiniciável esperando dados mostra o FIFO **vazio** (IFC = 0),
  como o hardware, que já teria consumido tudo. Sem isso o vídeo congelava
  no quadro 22: enquanto espera o IPU, a libmpeg faz `while (IFC == 0)
  callback_sem_dados()`, e é esse callback que acorda a thread que enche o
  anel e reinicia o DMA (achado com o relatório de threads abaixo).
- ✅ `ANYPS2_TRACE=threads`: ao encerrar por `ANYPS2_FRAMES`, o estado das
  threads do EE e de onde as bloqueadas chamaram a espera.
- Com isso **o vídeo de abertura toca** — fade do preto para o branco (os
  quadros batem com os de um decodificador de referência; o vídeo guarda
  os dois campos empilhados e o jogo os entrelaça na saída) com o logo da
  Polyphony Digital desenhado por cima pelo jogo. No fundo branco sobram
  padrões de 3 níveis (251 × 254), provavelmente arredondamento acumulado
  nos quadros P/B.
- ✅ IPU, etapa 5: **IDEC** — uma fatia intra inteira até RGB32:
  macroblock_type (I), dct_type com DTD, quantiser_scale_code quando o
  tipo pede, os 6 blocos com a predição do DC seguindo de um macrobloco ao
  outro (macrobloco pulado a zera), CSC com SGN, e o MBAI seguinte; um
  código que não é MBAI (o start code) encerra a fatia (SCD, TOP, CBP,
  CTRL.PCT = I). Reiniciável por macrobloco. Testes `ipu.idec_*`.
- ✅ Syscalls 0xFD/0xFF: a libkernel do SDK 3.0 chama `iSetAlarm`/
  `iReleaseAlarm` com o número positivo (teste
  `timing.alarm_syscall_numbers`).
- ✅ MPG1: vídeo em repetição (flag 0x10) — no fim do stream o leitor volta
  ao começo (como o IRX); uma passada sem nenhum pacote de vídeo encerra
  (teste `iop.movie_program_stream_loop`).
- Onde o GT4 está: com o roteiro de controle, depois da abertura entra no
  modo Arcade — **menu "Single Race" com os quatro fundos em vídeo**
  (World Circuits, Original Circuits, City Courses, Dirt & Snow; três
  vídeos em repetição decodificados pelo IPU), **Car Selection** (mapa dos
  fabricantes) e a **vitrine 3D de um carro (Lotus Elise)** — e para numa
  transferência **LOCAL→HOST do GS** (TRXDIR=1, ler a VRAM).
- ✅ **Transferência LOCAL→HOST do GS** (download da VRAM): com TRXDIR=1
  o retângulo de origem (SBP/SBW/SPSM, SSAX/SSAY, TRXREG) é empacotado como
  numa HOST→LOCAL do mesmo PSM e completado até quadword; com BUSDIR=1 sai
  pelo VIF1 — DMA do canal 1 com DIR=0 (modo normal) ou leitura do
  VIF1_FIFO. VIF1_STAT.FDR gravável. Sem BUSDIR, erro claro. Teste
  `gs.local_to_host_download`. O GT4 usa assim: o TRXDIR chega num DIRECT
  do VIF1 e a interrupção seguinte liga FDR/BUSDIR e o DMA de volta.
- Onde o GT4 está: passa da vitrine e **começa uma corrida** (Nürburgring,
  com o Lotus Elise, os adversários, o mapa da pista e o painel), lenta
  com o GS em software; até o quadro 11150 (limite de 25 min do
  `jogos/run_gt4.sh`) nenhum erro. Há defeitos de textura (partes dos
  carros pretas).
- ✅ Medição de desempenho (`ANYPS2_PROFILE=1`): tempo do host por parte e
  pares de VU compilados × interpretados. No GT4: menus 16–25 VBlanks/s
  (GS 60–80%), vitrine 2,4 (GS 55%, VU1 39%), corrida ~2 (GS 72–77%, VU1
  18–23%; o VU1 já roda todo compilado, ~2 M pares por quadro). Ajustes do
  rasterizador sem mudar nenhum pixel (arestas incrementais, só os
  atributos usados, sprite por coluna/linha, log2 do LOD só quando muda o
  filtro): ~8–10% nos menus. O microcódigo do VU1 da corrida entrou no
  projeto do GT4 com `--vu-dumps`.
- ✅ **GS numa thread própria** (`gs_worker.h/.cpp`, `gs_coverage.h/.cpp`):
  o EE valida cada desenho, conta os pixels pela conta analítica (o tempo
  do GS e o FINISH no relógio virtual não esperam o rasterizador) e
  enfileira; um worker executa desenhos, transferências e cargas de CLUT
  na ordem do GIF. Quem lê a VRAM pela API pública (LOCAL→HOST, display,
  SIGNAL, FINISH no relógio real, reset) espera o worker antes.
  `ANYPS2_GS_THREAD=0` volta ao caminho síncrono. No GT4 as capturas são
  idênticas byte a byte com e sem o worker, e a velocidade sobe 1,2–1,5×
  (menus 29,6 → 42,8 VBlanks/s; corrida 2,3 → 3,3). Na corrida o worker
  fica 95–98% ocupado: o próximo passo é rasterizar em faixas da tela com
  várias threads. O `ANYPS2_PROFILE` mostra o tempo do EE esperando o
  worker (GSesp) e a ocupação do worker.
- ✅ CSA também na amostragem de texturas de 8 bits (a carga da CLUT já o
  aplicava). O GT4 não usa esse caso até a largada: não é a causa das
  partes pretas dos carros, que parecem uma passada de reflexo da pintura.
- Listras nos vídeos de fundo: não reproduzidas. O GT4 roda em PAL
  entrelaçado com FFMD=1 (modo FRAME, framebuffer de meia altura) e troca o
  DISPFB a cada campo (renderização por campo); as capturas não têm padrão
  de linha. Na janela, a saída mostra um campo por VBlank (bob); se houver
  tremor, a correção é combinar os dois campos (weave).
- ✅ **GS em faixas** (`gs_bands.h/.cpp`, `gs_access.h`): `ANYPS2_GS_THREADS=N`
  threads (padrão de 2 a 4), cada uma dona dos blocos de 16 linhas
  (y/16) % N, com fila própria na ordem do GIF. Transferências, CLUT e reset
  são barreiras ordenadas. O produtor guarda as páginas da VRAM acessadas
  desde a última barreira: um desenho que lê (textura) ou escreve o que outro
  pendente escreve espera com uma barreira; um desenho cuja textura cai no
  próprio FRAME/ZBUF, ou cujo endereço dá a volta (x além de FBW·64), vai
  numa faixa só. No GT4, 91% dos desenhos vão para as faixas, as capturas
  são idênticas às do caminho síncrono e a corrida sobe de 4,8–7,0 para
  6,8–11,8 VBlanks/s (máquina livre; 1 faixa contra 4). Agora o VU1 é o
  gargalo na corrida (52–73% do tempo do EE).
- ✅ **ZTE=0 não escreve Z** (como no GSdx). O GT4 deixa o ZBUF no endereço
  do FRAME nos desenhos 2D; escrever Z apagava a cor: eram os polígonos
  pretos que cobriam a vitrine e o menu de opções antes da corrida.
- ✅ Sonda do GS: `ANYPS2_GS_PROBE=x,y` (estado de cada desenho que toca o
  pixel, com a cor e o Z antes e depois) e `ANYPS2_GS_DRAWLOG` (uma linha
  por desenho), para diagnosticar defeitos de imagem.
- Partes pretas dos carros (sonda no buffer 0xF0, onde a cena 3D é desenhada
  antes de ser copiada para a tela): a pintura entra escura (cor de vértice
  19,19,20 em MODULATE) e um triângulo grande com cor de vértice (0,0,0) e Z
  maior que o da carroceria (GEQUAL) cobre o pixel. As cores e o Z vêm do
  microcódigo do VU1: o próximo passo é conferir a saída do VU1 desses desenhos.
- Próximos passos: as partes pretas dos carros (com a sonda); o VU1 da
  corrida; o cache de texturas. O som: ver o marco 8.

Marco 8 — os primeiros sons: driver de som da Polyphony (PDISPU2) em HLE ✅
(efeitos; a música de fundo e o áudio dos vídeos ainda não):
- ✅ **O caminho do som não exige executar o IOP.** O levantamento do
  `PDISPU2.IRX` ("PDI_SPU2_Manager" v1.18; `libsd` e `libpdi` importados)
  mostrou um módulo que só traduz uma cópia dos registradores do SPU2
  mantida pelo EE em chamadas ao libsd (SetParam/SetSwitch/SetAddr) e
  move dados pelo FIFO do `libpdi` — tudo coberto pelo `Spu2` do runtime.
  O protocolo está documentado no topo de `runtime/src/iop/pdispu2.cpp`.
- ✅ **SPUP / SPUT** (`pdispu2.cpp`, `pdispu2.h`): fn 4 aplica o bloco de
  960 bytes (2 núcleos × 464: marcas por voz e por núcleo, key-on/key-off
  em máscaras, ADSR, pitch, volumes, endereço inicial) e responde com o
  estado (ENDX + ENVX das 48 vozes); fn 3 só o estado; fn 1/2 leem/escrevem
  a RAM do SPU2 de/para o EE; fn 5/6 (modo da saída digital) são aceitas sem
  efeito. Reverb, PMON, NON e volume em "sweep" são avisados na primeira
  vez e ignorados (o `Spu2` ainda não os tem).
- ✅ **pdistr fn 5**: o destino é a RAM do SPU2 (o IRX chama `pdispu2_35`,
  que envia por `sceSdVoiceTrans`) — os bancos de som do jogo
  (0x5040…0x1e0000) agora chegam ao SPU2 em vez de cair na RAM do IOP.
- ✅ `Spu2`: `envelopeLevel` (ENVX), `reachedEnd` (ENDX) e `setEnvelope`.
- ✅ Testes `iop.pdispu2_*` (bloco de registradores → vozes: key-on com
  endereço/pitch/volume/ADSR, marca "zerar o volume", máscara de vozes,
  key-off × key-on, mudança em voz tocando, estado ENDX/ENVX, bloco curto).
- Onde o GT4 está: o WAV de uma partida de ~50 s (3000 quadros) tem os
  efeitos dos menus (cliques e confirmações) com picos de ~10 000.
- Falta: **PBGM** (`PBGM`, música de fundo em streaming: fn 1–9, a
  mesma biblioteca `pdispu2` com voz de streaming e `sceSdBlockTrans`),
  **VOIC**, o áudio dos vídeos (stream privado 0xBD do MPG1/MPG2, também via
  `pdispu2`), reverb, os servidores do USB (`bsuP`, `THUP`...) e o
  servidor `0x8000059c`.

- ⬜ A partir de um dump do usuário: análise do ELF principal e de overlays
  (muitos jogos carregam código extra do disco), configuração TOML por jogo.
- ⬜ Lista de compatibilidade (`docs/COMPATIBILIDADE.md`) com status por jogo.
- ⬜ Correções jogo a jogo, sempre com teste de regressão.

Riscos (alto): mesmo um jogo "simples" exercita quase todo o hardware. É
realista esperar que o primeiro jogo comercial jogável leve muito mais tempo
que as fases anteriores somadas; overlays de código carregados do disco
exigem recompilar também esses binários (a partir do mesmo dump).

## Configuração do host (anyps2.ini) 🚧

Primeiro passo da UI de configuração: a camada de configuração, ainda sem
interface gráfica. A ordem é **variável de ambiente > arquivo > padrão**, e
sem arquivo o comportamento é o de sempre.

- ✅ Arquivo INI escrito à mão, sem dependência nova (`ANYPS2_CONFIG` ou
  `anyps2.ini` ao lado do executável): `[memcard] dir`, `[disc] iso`,
  `[video] mode/scale`, `[audio] mode/wav`, e as ligações de teclado
  (`[keyboard.1]`, `[keyboard.2]`) e de controle SDL (`[gamepad.1]`,
  `[gamepad.2]`) para cada botão do DS2 e para os analógicos lx ly rx ry.
  Erros de sintaxe dizem arquivo, linha e motivo.
- ✅ A janela usa as ligações configuradas (padrões = o mapeamento fixo de
  antes). Nome de tecla, botão ou eixo desconhecido é erro com a linha
  (conferido com o SDL; sem SDL, aceito). Eixo digital aceita sentido
  (`+leftx`, `-leftx`).
- ✅ Novas variáveis: `ANYPS2_CONFIG`, `ANYPS2_VIDEO_SCALE`, `ANYPS2_MC_DIR`
  (esta última antes lida direto no memory card).
- ✅ Testes: leitura e comentários, erros com linha, prioridade
  env > arquivo > padrão, e padrões iguais ao mapeamento fixo (`config.*`).
- ✅ Menu dentro da janela (F1, Dear ImGui 1.90.9 via CMake: `ANYPS2_FETCH_IMGUI`
  ou `ANYPS2_IMGUI_DIR`; só com SDL): ligações por porta com "aperte a tecla ou
  o botão" (Esc cancela), janela e som, cartões, disco e "Salvar anyps2.ini".
  Com o menu aberto o jogo recebe controle neutro. A gravação é testada (ler →
  gravar → ler); a janela em si precisa de teste manual.
- ⬜ Escolher a ISO e a pasta dos cartões por diálogo de arquivos (hoje é digitação).
