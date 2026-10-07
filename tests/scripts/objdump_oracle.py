#!/usr/bin/env python3
"""Oráculo diferencial: compara o decodificador do AnyPS2 com o GNU objdump.

O GNU binutils (>= 2.31) conhece o conjunto de instruções do EE com
`-m mips:5900`: MIPS III + MMI + COP1 do R5900 + macro-instruções do VU0.
Ele é uma implementação independente da nossa, então serve de oráculo.

Uso:
  objdump_oracle.py golden [--out ARQ] [--per-slot N] [--random N] [--seed S]
      Gera o arquivo golden versionado (tests/data/r5900_golden.txt).

  objdump_oracle.py check --tests CAMINHO/anyps2_tests [--per-slot N] [--random N]
      Gera um corpus grande (não versionado), roda o objdump e valida com a
      mesma lógica do teste C++ (suíte "golden", via ANYPS2_GOLDEN_FILE).

Requer `mipsel-linux-gnu-objdump` (pacote binutils-mipsel-linux-gnu) ou
outro objdump indicado em --objdump.
"""

import argparse
import os
import random
import struct
import subprocess
import sys
import tempfile

BASE_ADDRESS = 0x00100000

# Blocos de 5/6 bits que podem ser zerados aleatoriamente para gerar
# codificações canônicas (os campos rs, rt, rd, sa e funct).
CHUNKS = [0x03E00000, 0x001F0000, 0x0000F800, 0x000007C0, 0x0000003F]


def slots():
    """Todas as posições das tabelas de opcode: (seletor, máscara_livre)."""
    s = []
    low26 = 0x03FFFFFF
    for op in range(64):
        if op not in (0, 1, 16, 17, 18, 28):
            s.append((op << 26, low26))
    for f in range(64):  # SPECIAL
        s.append((f, 0x03FFFFC0))
    for rt in range(32):  # REGIMM
        s.append(((1 << 26) | (rt << 16), 0x03E0FFFF))
    for f in range(64):  # MMI
        if f not in (8, 9, 40, 41, 48, 49):
            s.append(((28 << 26) | f, 0x03FFFFC0))
    for fn in (8, 9, 40, 41, 48, 49):  # MMI0..3, PMFHL, PMTHL
        for sa in range(32):
            s.append(((28 << 26) | (sa << 6) | fn, 0x03FFF800))
    for cop in (16, 17, 18):
        for rs in range(32):
            if rs & 0x10 and cop != 16:
                continue
            if rs == 8:
                for rt in range(32):  # BCz
                    s.append(((cop << 26) | (8 << 21) | (rt << 16), 0x0000FFFF))
            elif rs != 16 or cop != 16:
                s.append(((cop << 26) | (rs << 21), 0x001FFFFF))
    for f in range(64):  # COP0 C0
        s.append(((16 << 26) | (16 << 21) | f, 0x01FFFFC0))
    for rd in (24, 25):  # COP0 debug / performance
        for rs in (0, 4):
            for f in range(64):
                s.append(((16 << 26) | (rs << 21) | (rd << 11) | f, 0x001F07C0))
    for fmt in range(16, 32):  # COP1 formatos
        for f in range(64):
            s.append(((17 << 26) | (fmt << 21) | f, 0x001FFFC0))
    for f in range(60):  # COP2 Special1
        s.append(((18 << 26) | (1 << 25) | f, 0x01FFFFC0))
    for i in range(128):  # COP2 Special2
        s.append(((18 << 26) | (1 << 25) | ((i >> 2) << 6) | 0x3C | (i & 3), 0x01FFF800))
    return s


def corpus(per_slot, random_words, seed):
    rng = random.Random(seed)
    words = []
    for base, free in slots():
        for _ in range(per_slot):
            w = base | (rng.getrandbits(32) & free)
            for chunk in CHUNKS:
                if rng.random() < 0.5:
                    w &= ~(chunk & free)
            words.append(w & 0xFFFFFFFF)
    for _ in range(random_words):
        words.append(rng.getrandbits(32))
    return words


def run_objdump(objdump, words):
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "corpus.bin")
        with open(path, "wb") as f:
            f.write(b"".join(struct.pack("<I", w) for w in words))
        out = subprocess.run(
            [objdump, "-D", "-z", "-b", "binary", "-m", "mips:5900", "-EL", "-M", "no-aliases",
             "--adjust-vma=%#x" % BASE_ADDRESS, path],
            check=True, capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) < 3 or not parts[0].strip().endswith(":"):
            continue
        addr = int(parts[0].strip()[:-1], 16)
        word = int(parts[1].strip(), 16)
        text = "\t".join(parts[2:]).rstrip()
        result[addr] = (word, text)
    lines = []
    for i, w in enumerate(words):
        addr = BASE_ADDRESS + 4 * i
        if addr not in result or result[addr][0] != w:
            sys.exit("saída inesperada do objdump no endereço %#x" % addr)
        lines.append("%08x %08x\t%s" % (addr, w, result[addr][1]))
    return lines


def objdump_version(objdump):
    return subprocess.run([objdump, "--version"], capture_output=True,
                          text=True).stdout.splitlines()[0]


def write_golden(path, lines, objdump, args):
    with open(path, "w", newline="\n") as f:
        f.write("# Golden do decodificador R5900 gerado por tests/scripts/objdump_oracle.py\n")
        f.write("# Oráculo: %s (-m mips:5900 -M no-aliases)\n" % objdump_version(objdump))
        f.write("# Parâmetros: per_slot=%d random=%d seed=%d\n"
                % (args.per_slot, args.random, args.seed))
        f.write("# Formato: <endereço> <palavra>\\t<texto do objdump>\n")
        for line in lines:
            f.write(line + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["golden", "check"])
    ap.add_argument("--objdump", default="mipsel-linux-gnu-objdump")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "data",
                                                  "r5900_golden.txt"))
    ap.add_argument("--tests", help="caminho do executável anyps2_tests (modo check)")
    ap.add_argument("--per-slot", type=int, default=None)
    ap.add_argument("--random", type=int, default=None)
    ap.add_argument("--seed", type=int, default=5900)
    args = ap.parse_args()

    if args.mode == "golden":
        args.per_slot = 6 if args.per_slot is None else args.per_slot
        args.random = 1000 if args.random is None else args.random
        lines = run_objdump(args.objdump, corpus(args.per_slot, args.random, args.seed))
        write_golden(args.out, lines, args.objdump, args)
        print("golden com %d palavras gravado em %s" % (len(lines), os.path.normpath(args.out)))
        return 0

    if not args.tests:
        ap.error("--tests é obrigatório no modo check")
    args.per_slot = 200 if args.per_slot is None else args.per_slot
    args.random = 200000 if args.random is None else args.random
    words = corpus(args.per_slot, args.random, args.seed)
    print("corpus: %d palavras; rodando objdump..." % len(words))
    lines = run_objdump(args.objdump, words)
    with tempfile.TemporaryDirectory() as tmp:
        path = os.path.join(tmp, "golden_big.txt")
        write_golden(path, lines, args.objdump, args)
        env = dict(os.environ, ANYPS2_GOLDEN_FILE=path)
        return subprocess.run([args.tests, "golden"], env=env).returncode


if __name__ == "__main__":
    sys.exit(main())
