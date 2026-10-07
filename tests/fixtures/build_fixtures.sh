#!/bin/sh
# Regenera os ELFs de fixture a partir do assembly (requer
# binutils-mipsel-linux-gnu). Os binários gerados são versionados para que
# os testes não dependam do binutils.
set -eu
cd "$(dirname "$0")"
AS=${AS:-mipsel-linux-gnu-as}
LD=${LD:-mipsel-linux-gnu-ld}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$AS" -march=r5900 -mabi=eabi -EL -o "$tmp/hello_r5900.o" hello_r5900.s
"$LD" -EL -n -T hello_r5900.ld -o hello_r5900.elf "$tmp/hello_r5900.o"
echo "hello_r5900.elf gerado"

# Listagem esperada da .text, produzida pelo GNU objdump (modo binário, sem
# símbolos), usada pelo teste "fixture" para comparar com o nosso
# disassembler.
OBJCOPY=${OBJCOPY:-mipsel-linux-gnu-objcopy}
OBJDUMP=${OBJDUMP:-mipsel-linux-gnu-objdump}
"$OBJCOPY" -O binary -j .text hello_r5900.elf "$tmp/text.bin"
"$OBJDUMP" -D -z -b binary -m mips:5900 -EL -M no-aliases --adjust-vma=0x100000 "$tmp/text.bin" |
    awk -F'\t' '/^ *[0-9a-f]+:\t/ { sub(/^ */, "", $1); sub(/ *$/, "", $2); printf "%s %s", substr($1, 1, length($1) - 1), $2; for (i = 3; i <= NF; i++) printf "\t%s", $i; printf "\n" }' \
    > hello_r5900.text.expected
echo "hello_r5900.text.expected gerado"
