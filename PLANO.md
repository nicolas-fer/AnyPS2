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
- FPU: o PS2 arredonda em direção a zero; usamos o arredondamento do host
  (para o mais próximo). Resultados podem diferir no último bit. Valores com
  expoente 255 (que no PS2 são números normais) são aproximados por ±Fmax.
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

## Fase 4 — Gráficos: GIF/VIF/DMA + Graphics Synthesizer ⬜

- ⬜ DMAC (canais VIF0, VIF1, GIF, fromIPU/toIPU, SIF0–2, SPR): modos normal,
  chain (tags `cnt/next/ref/refs/call/ret/end`) e interleave.
- ⬜ GIF: PATH1/2/3, GIFtag (PACKED/REGLIST/IMAGE), registradores A+D.
- ⬜ VIF0/VIF1: desempacotamento (`UNPACK` V1–V4, 8/16/32 bits, máscaras),
  `MSCAL/MSCNT`, `DIRECT/DIRECTHL`, `STCYCL`, `STMOD`...
- ⬜ GS: registradores privilegiados (PMODE, DISPFB, DISPLAY, CSR/IMR),
  primitivas (pontos, linhas, triângulos, strips, fans, sprites), texturas
  (PSMCT32/24/16, PSMT8/4 com CLUT), alpha blending, Z-test, scissor,
  transferências HOST↔LOCAL e LOCAL↔LOCAL, swizzle da VRAM de 4 MB.
- ⬜ Backend: renderizador por hardware (Vulkan **ou** OpenGL 4.x — decisão
  no início da fase) + renderizador de referência por software para testes.
  Janela e apresentação via SDL.
- ⬜ Testes: dumps de GS (formato nosso) com imagem esperada; homebrew 2D
  (sprites, texto), depois 3D (cubo com Z-buffer e textura).

Riscos (alto): o GS é o componente mais difícil de emular com precisão.
Efeitos que leem o framebuffer como textura, formatos de VRAM entrelaçados e
precisão de blending exigem muito trabalho; o PCSX2 levou anos. A meta da
fase é homebrew, não jogos.

## Fase 5 — VU0/VU1 e recompilação de microcódigo ⬜

- ⬜ Decodificador do microcódigo (pares upper/lower de 64 bits, bits I/E/M/D/T).
- ⬜ Interpretador de referência (com pipeline: Q/P, stalls, flags MAC/status/clip
  atrasados) para validar.
- ⬜ Recompilação: microprogramas encontrados em tempo de geração (dados do
  ELF enviados por VIF `MPG`) e cache por hash para os enviados em tempo de
  execução (recompilação AOT a partir de dumps + fallback interpretado).
- ⬜ XGKICK (PATH1) para o GIF, VU0 em modo micro (`VCALLMS`).
- ⬜ Testes: vetores por instrução do VU + microprogramas de homebrew.

Riscos (alto): microcódigo é carregado dinamicamente pelo jogo; parte dele
só é conhecida em tempo de execução, então um interpretador sempre será
necessário como rede de segurança.

## Fase 6 — IOP em HLE ⬜

- ⬜ SIF (DMA EE↔IOP, `sceSifCallRpc`, `sceSifBindRpc`, `SifLoadModule`).
- ⬜ Módulos em HLE por interface RPC: `sio2man`/`padman`/`xpadman` (pad via
  SDL GameController), `mcman`/`mcserv` (memory card em arquivo), `cdvdman`/
  `cdvdfsv` (leitura de ISO própria, ISO9660), `libsd`/`sdrdrv` (SPU2),
  `ioman`/`fileio`.
- ⬜ SPU2: 48 vozes ADPCM, envelopes ADSR, reverb, saída via SDL audio.
- ⬜ Módulo IRX desconhecido → erro com nome do módulo e versão.

Riscos (médio/alto): cada jogo usa versões diferentes dos módulos e alguns
carregam drivers IRX próprios (áudio, streaming). Para esses, HLE por nome
não basta e será preciso emular/recompilar o próprio IOP (R3000A) — o
parser de ELF já reconhece IRX, mas isso é um subprojeto de porte similar à
Fase 2.

## Fase 7 — Primeiro jogo comercial ⬜

- ⬜ A partir de um dump do usuário: análise do ELF principal e de overlays
  (muitos jogos carregam código extra do disco), configuração TOML por jogo.
- ⬜ Lista de compatibilidade (`docs/COMPATIBILIDADE.md`) com status por jogo.
- ⬜ Correções jogo a jogo, sempre com teste de regressão.

Riscos (alto): mesmo um jogo "simples" exercita quase todo o hardware. É
realista esperar que o primeiro jogo comercial jogável leve muito mais tempo
que as fases anteriores somadas; overlays de código carregados do disco
exigem recompilar também esses binários (a partir do mesmo dump).
