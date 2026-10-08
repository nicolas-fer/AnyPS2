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

# Amostras do próprio ps2sdk (arquivo SAMPLE com o caminho em $PS2SDK/samples),
# sem alterações. A imagem não tem make: seguimos o Makefile da amostra
# (EE_OBJS, EE_LIBS, regras bin2c e o strip final).
for dir in "$here"/*/; do
    name=$(basename "$dir")
    [ -f "$dir/SAMPLE" ] || continue
    work="/tmp/sample-$name"
    rm -rf "$work"
    mkdir -p "$work"
    cp -r "$PS2SDK/samples/$(cat "$dir/SAMPLE")/." "$work/"
    mk="$work/Makefile"
    grep -E '^[[:space:]]*bin2c ' "$mk" | while read -r cmd a b c; do
        (cd "$work" && "$PS2SDK/bin/bin2c" "$a" "$b" "$c")
    done
    objs=""
    for o in $(sed -n 's/^EE_OBJS[[:space:]]*=//p' "$mk"); do
        base="${o%.o}"
        if [ -f "$work/$base.vsm" ]; then
            "$PS2DEV/dvp/bin/dvp-as" -o "$work/$o" "$work/$base.vsm"
        else
            # shellcheck disable=SC2086
            $CC -D_EE -G0 -O2 -Wall -I$PS2SDK/ee/include -I$PS2SDK/common/include -I"$work" -c "$work/$base.c" -o "$work/$o"
        fi
        objs="$objs $work/$o"
    done
    libs=$(sed -n 's/^EE_LIBS[[:space:]]*=//p' "$mk")
    # shellcheck disable=SC2086
    $CC $LDFLAGS -o "$dir/$name.elf" $objs $libs
    if grep -q -- '--strip-all' "$mk"; then
        mips64r5900el-ps2-elf-strip --strip-all "$dir/$name.elf"
    else
        mips64r5900el-ps2-elf-strip --strip-debug "$dir/$name.elf"
    fi
    echo "gerado: $name/$name.elf (amostra do ps2sdk: $(cat "$dir/SAMPLE"))"
done

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
    # Microcódigo dos VUs (.vsm) com o montador do ps2dev.
    for src in "$dir"/*.vsm; do
        [ -f "$src" ] || continue
        obj="/tmp/$name-$(basename "$src").o"
        "$PS2DEV/dvp/bin/dvp-as" -o "$obj" "$src"
        objs="$objs $obj"
    done
    libs=""
    [ -f "$dir/LIBS" ] && libs=$(eval echo "$(cat "$dir/LIBS")")
    # LOADADDR opcional: endereço do .text no lugar do 0x00100000 do linkfile
    # (ex.: um "boot" que carrega outro programa em 0x00100000).
    ldflags="$LDFLAGS"
    if [ -f "$dir/LOADADDR" ]; then
        sed "s/\.text 0x00100000:/.text $(cat "$dir/LOADADDR"):/" "$PS2SDK/ee/startup/linkfile" > "/tmp/$name.ld"
        ldflags="-T/tmp/$name.ld -O2 -L$PS2SDK/ee/lib -Wl,-zmax-page-size=128"
    fi
    # shellcheck disable=SC2086
    $CC $ldflags -o "$dir/$name.elf" $objs $libs
    # Mantém os símbolos (úteis para a análise), remove só a informação de debug.
    mips64r5900el-ps2-elf-strip --strip-debug "$dir/$name.elf"
    echo "gerado: $name/$name.elf"
done
