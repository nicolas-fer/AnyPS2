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

Regenerar (requer Docker; usa a imagem `ghcr.io/ps2dev/ps2dev`):

```sh
tests/homebrew/build.sh
```

Os ELFs são linkados com o ps2sdk (Academic Free License 2.0) e o newlib
(licenças BSD) do ps2dev; nenhum código da Sony está envolvido.
