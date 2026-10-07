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

## Fase 2 — Gerador de C++ + runtime mínimo 🔜

Meta: um homebrew de console (printf) compilado com o ps2dev rodando nativo.

### 2.1 Análise (recompiler/analysis)
- ⬜ Descoberta de funções: símbolos `STT_FUNC`; alvos de `JAL`; entry point;
  ponteiros de função em `.data`/`.rodata` (palavras que apontam para início
  plausível de código); heurística de prólogo (`addiu sp,sp,-N`) para ELFs
  sem símbolos.
- ⬜ Limites de função e blocos básicos; alvos de desvio dentro da função
  viram rótulos.
- ⬜ Jump tables: reconhecer o padrão `sltiu/beq → sll → lui/addiu → addu → lw → jr`
  e ler a tabela do ELF; `jr` não resolvido vira despacho dinâmico.
- ⬜ Arquivo de configuração TOML por jogo (funções extras, funções a pular,
  stubs por nome/endereço, patches de instrução), gerado/editável.

### 2.2 Gerador (recompiler/codegen)
- ⬜ Uma função C++ por função MIPS: `void f_00100024(Context* ctx)`.
- ⬜ Tradução instrução a instrução para C++ legível (macros/inline do runtime).
- ⬜ Delay slots: avaliar a condição antes, executar o slot, depois desviar;
  likely: slot só no caminho tomado. Desvio para o meio de outra função →
  chamada + retorno (tail call).
- ⬜ `JAL` para função conhecida → chamada direta; `JALR`/`JR` não-ra →
  `ctx->dispatch(endereço)` via tabela de funções (erro claro se o endereço
  não for uma função conhecida).
- ⬜ Instruções inválidas/não suportadas → erro em tempo de geração com
  endereço e palavra; opcionalmente `throw` em tempo de execução se estiver
  em código nunca alcançável comprovadamente.
- ⬜ Saída: projeto CMake pronto (`anyps2 recomp jogo.elf -o saida/`), com
  arquivos divididos por tamanho para compilar em paralelo.

### 2.3 Runtime mínimo (runtime/)
- ⬜ `Context`: 32 GPRs de 128 bits (portável: struct com 2×u64, sem
  `__int128`, que o MSVC não tem), HI/LO/HI1/LO1, SA, PC, FPU (32 regs +
  ACC + FCR31), COP0 básico.
- ⬜ Memória: 32 MB de RDRAM com espelhos (kuseg, kseg0/1, 0x2000_0000 e
  0x3000_0000), scratchpad de 16 KB em 0x7000_0000, região de I/O com
  despacho para handlers (acesso não tratado → erro com endereço e tamanho).
- ⬜ Carregador: copia PT_LOAD, zera bss, monta argc/argv, pilha e heap.
- ⬜ Syscalls básicas do kernel do EE em HLE: `Exit`, `SetupThread`,
  `SetupHeap`, `EndOfHeap`, `FlushCache`, `GsPutIMR`/`GsGetIMR`, `GetThreadId`,
  `CreateSema`... (lista guiada pelo que o crt0 do ps2sdk chama). Syscall
  desconhecida → erro com número e PC.
- ⬜ FPU com semântica do PS2 (sem NaN/Inf, saturação, flags) — inicialmente
  IEEE com verificação, com modo "exato" opcional.

### 2.4 Testes
- ⬜ Testes por instrução **executados**: cada instrução gerada roda contra
  vetores de entrada/saída (incl. MMI e casos de borda: overflow, divisão por
  zero, shifts ≥ 32).
- ⬜ Homebrew de ponta a ponta compilado com o ps2dev (imagem Docker oficial
  no CI): `hello` (printf), aritmética 64/128 bits, ponto flutuante,
  `setjmp/longjmp`, ponteiros de função, `switch` com jump table.
  Saída comparada com o esperado.

### Riscos da Fase 2 (avise antes de seguir)
- **printf do ps2sdk não é só uma syscall**: no EE, `printf` vai por SIF RPC
  para o módulo `ioman`/`fileio` do IOP (`tty:`). Para a meta da fase vamos
  precisar de um HLE mínimo de SIF RPC (servidor fileio só com `write` em
  `tty:`/`host:`) — um pedaço pequeno da Fase 6 antecipado.
- Código não-estruturado (setjmp/longjmp, troca de contexto, retorno para
  endereço calculado) não cabe em "uma função C++ por função MIPS"; precisa
  de saída por exceção/despacho, que será testada com homebrew específico.

## Fase 3 — Kernel do EE: threads, semáforos, timers, interrupções ⬜

- ⬜ Threads do EE (prioridade, `StartThread`, `SleepThread`, `WakeupThread`,
  `RotateThreadReadyQueue`, `ChangeThreadPriority`, `iWakeupThread`...).
  Como o código recompilado usa a pilha do host, cada thread do EE precisa
  de uma pilha própria: threads do host com "bastão" (só uma roda por vez,
  escalonamento decidido pelo nosso kernel), o que é portável entre
  Windows/Linux sem assembly.
- ⬜ Semáforos e event flags (`CreateSema`, `WaitSema`, `SignalSema`, `PollSema`,
  versões `i*` de interrupção).
- ⬜ Alarmes e timers (`SetAlarm`, `ReleaseAlarm`), contadores T0–T3.
- ⬜ INTC/DMAC: `AddIntcHandler`, `AddDmacHandler`, `EnableIntc`, VBlank
  (start/end) sintético a 50/60 Hz.
- ⬜ Testes: homebrew com produtor/consumidor, prioridades, alarmes,
  handler de VBlank.

Riscos: jogos que dependem de temporização exata entre threads/interrupções.
Começamos com um relógio virtual determinístico (útil para testes) e
avançamos para tempo real.

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
