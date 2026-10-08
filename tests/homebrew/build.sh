#!/bin/sh
# Compila os homebrews de teste com o toolchain oficial do ps2dev.
#
#   tests/homebrew/build.sh            # usa Docker (imagem ghcr.io/ps2dev/ps2dev)
#   PS2DEV=/caminho tests/homebrew/build.sh --native   # toolchain já instalado
#
# Cada subdiretório com main.c vira <nome>/<nome>.elf. As flags espelham
# $PS2SDK/samples/Makefile.eeglobal (não dependemos de make dentro da imagem).
# Os ELFs gerados são versionados: os testes não precisam do ps2dev.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
image=${PS2DEV_IMAGE:-ghcr.io/ps2dev/ps2dev:latest}

if [ "${1:-}" != "--native" ]; then
    exec docker run --rm -v "$here:/src" -w /src "$image" sh /src/build.sh --native
fi

: "${PS2SDK:=$PS2DEV/ps2sdk}"
CC=mips64r5900el-ps2-elf-gcc
CFLAGS="-D_EE -G0 -O2 -Wall -Werror -I$PS2SDK/ee/include -I$PS2SDK/common/include"
LDFLAGS="-T$PS2SDK/ee/startup/linkfile -O2 -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128"

for dir in "$here"/*/; do
    name=$(basename "$dir")
    [ -f "$dir/main.c" ] || continue
    objs=""
    # CFLAGS/LIBS opcionais por homebrew (podem usar $PS2DEV/$PS2SDK).
    extra=""
    [ -f "$dir/CFLAGS" ] && extra=$(eval echo "$(cat "$dir/CFLAGS")")
    for src in "$dir"/*.c "$dir"/*.S; do
        [ -f "$src" ] || continue
        obj="/tmp/$name-$(basename "$src").o"
        # shellcheck disable=SC2086
        $CC $CFLAGS $extra -c "$src" -o "$obj"
        objs="$objs $obj"
    done
    libs=""
    [ -f "$dir/LIBS" ] && libs=$(eval echo "$(cat "$dir/LIBS")")
    # shellcheck disable=SC2086
    $CC $LDFLAGS -o "$dir/$name.elf" $objs $libs
    # Mantém os símbolos (úteis para a análise), remove só a informação de debug.
    mips64r5900el-ps2-elf-strip --strip-debug "$dir/$name.elf"
    echo "gerado: $name/$name.elf"
done
