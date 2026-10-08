# Homebrews de teste

Programas de teste nossos, compilados com o toolchain aberto do
[ps2dev](https://github.com/ps2dev/ps2dev) (GCC para `mips64r5900el-ps2-elf`).
Os ELFs gerados ficam versionados para que os testes não dependam do ps2dev.

| Diretório | O que testa | Saída esperada |
|---|---|---|
| `hello/` | printf via SIF RPC/fileio, argv | `expected.txt` |
| `cputest/` | CPU: inteiros, fluxo, libc, float + asm do R5900 (MMI, FPU, delay slots) | o mesmo `main.c` compilado para o host |
| `threads/` | escalonador: prioridades, semáforos, sleep/wakeup, terminate | `expected.txt` |
| `fileio/` | `host:` com fopen/fgets/fseek | `expected.txt` |
| `timers/` | DelayThread (Timer 2), SetAlarm, VBlank, laço sem syscall, preempção por interrupção | `expected.txt` |
| `gfx2d/` | libgraph + libdraw + libdma: sprites, Gouraud, leque, linhas, pontos, blending, textura PSMT8+CLUT via DMA chain | `expected.txt` + `expected.png` |
| `cube3d/` | cubo 3D: Z-buffer, textura em perspectiva (STQ), bilinear, iluminação, double buffering | `expected.txt` + `expected.png` |
| `gskit/` | gsKit + dmaKit: fila de desenho, textura, Gouraud, blending, `gsKit_sync_flip` | `expected.txt` + `expected.png` |
| `modules/` | `SifLoadModule("rom0:...")`, `SifExecModuleBuffer` com `padman.irx` embutido; `unknown`/`rom`: IRX/ROM sem HLE → erro claro | `expected*.txt` |
| `pad/` | libpad (XPADMAN; `rom`: PADMAN da ROM): modos, pressão, botões e analógicos do roteiro `script.txt`, desconexão | `expected.txt`, `expected_rom.txt` |
| `memcard/` | libmc: info, mkdir, escrita/leitura/seek, getDir, chdir, rename, delete | `expected.txt` |
| `cdvd/` | libcdvd e `cdrom0:` sobre `disc.iso` (gerado por `make_iso.py`); sem ISO → erro claro | `expected.txt`, `expected_noiso.txt` |
| `audio/` | `audsrv.irx` + `freesd.irx` do ps2sdk embutidos: PCM 22050 Hz e sample ADPCM com pan | `expected.txt` + `expected.wav` |
| `pdi/` | drivers da Polyphony (pdicdvd, pdistr, lgdev) e o SIF RPC multi-thread (msifrpc) com IRX mínimos só com o nome, sobre `cdvd/disc.iso` | `expected.txt` |

Regenerar (requer Docker; usa a imagem `ghcr.io/ps2dev/ps2dev`):

```sh
tests/homebrew/build.sh
```

Arquivos opcionais por diretório: `LIBS` (bibliotecas extras do link) e
`CFLAGS` (podem usar `$PS2DEV`/`$PS2SDK`, ex.: os includes da gsKit). IRX do
ps2sdk são embutidos com `.incbin` num `.S` (`CFLAGS` com
`-Wa,-I$PS2SDK/iop/irx`).

`expected.wav` é o som gravado com `ANYPS2_AUDIO_WAV` no relógio virtual,
conferido ao ser criado (frequências, duração e pan) e comparado byte a
byte pelo teste.

`expected.png` é a última imagem exibida pelo homebrew recompilado
(`ANYPS2_SCREENSHOT`), conferida visualmente ao ser criada; o teste exige
que a saída atual seja idêntica byte a byte. Se uma mudança intencional no GS
alterar a imagem, regenere com o executável do teste
(`build/tests/e2e/<nome>/build/<nome>`) e confira a diferença antes de
versionar.

Os ELFs são linkados com o ps2sdk (Academic Free License 2.0), a gsKit (AFL
2.0) e o newlib (licenças BSD) do ps2dev; nenhum código da Sony está
envolvido.
