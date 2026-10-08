#!/usr/bin/env python3
"""Golden diferencial do decodificador de microcódigo do VU.

Gera pares (lower, upper) aleatórios — metade montada a partir dos campos de
cada instrução, metade palavras totalmente aleatórias — e desmonta com o
dvp-objdump do ps2dev (binutils com suporte ao VU, `-m dvp:vu`). O resultado
vira tests/data/vu_golden.txt, que os testes unitários comparam com o nosso
disassembler. O dvp-objdump só existe no toolchain do ps2dev, então o golden
é versionado (regere com este script; requer Docker).

    python3 tests/scripts/vu_oracle.py golden [--count N] [--seed S]
"""
import argparse
import os
import random
import re
import struct
import subprocess
import sys
import tempfile

IMAGE = os.environ.get("PS2DEV_IMAGE", "ghcr.io/ps2dev/ps2dev:latest")
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Opcodes de 7 bits do lower (bits 25..31) e índices das tabelas estendidas.
LOWER_OPS = [0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
             0x18, 0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F]
LOWER_SPECIAL = [0x30, 0x31, 0x32, 0x34, 0x35]
LOWER_EXT = [0x30, 0x31, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
             0x40, 0x41, 0x42, 0x43, 0x64, 0x68, 0x69, 0x6C, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
             0x76, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E]
UPPER_EXT = list(range(0x00, 0x20)) + [0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29,
                                       0x2A, 0x2C, 0x2D, 0x2E, 0x2F]


def rnd_fields(r):
    return (r.getrandbits(4) << 21) | (r.getrandbits(5) << 16) | (r.getrandbits(5) << 11) | (r.getrandbits(5) << 6)


def sparse(r, value, zero_mask):
    """Com 70% de chance zera os campos em zero_mask (formas canônicas)."""
    return value & ~zero_mask if r.random() < 0.7 else value


def gen_upper(r):
    k = r.random()
    flags = 0
    if r.random() < 0.1:
        flags = r.choice([1 << 30, 1 << 29, 1 << 28, 1 << 27, (1 << 30) | (1 << 29)])
    if k < 0.05:
        return 0x000002FF | flags, False
    if k < 0.6:
        f = r.randrange(0, 0x30)
        w = rnd_fields(r) | f
        if 0x1C <= f <= 0x27:
            w = sparse(r, w, 0x1F << 16)
        return w | flags, False
    if k < 0.95:
        idx = r.choice(UPPER_EXT)
        w = (r.getrandbits(4) << 21) | (r.getrandbits(5) << 16) | (r.getrandbits(5) << 11)
        w |= ((idx >> 2) << 6) | 0x3C | (idx & 3)
        if idx in (0x1F, 0x2E):
            w = (w & ~(0xF << 21)) | (0xE << 21) if r.random() < 0.7 else w
        if idx in (0x1C, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27):
            w = sparse(r, w, 0x1F << 16)
        if idx == 0x2F:
            w = sparse(r, w, (0xF << 21) | (0x1F << 16) | (0x1F << 11))
        return w | flags, False
    return r.getrandbits(32), False


def gen_lower(r, iflag):
    if iflag:
        return struct.unpack("<I", struct.pack("<f", r.choice([1.0, -2.5, 0.125, 3.0e10, r.uniform(-100, 100)])))[0]
    k = r.random()
    if k < 0.05:
        return 0x8000033C
    if k < 0.45:
        op = r.choice(LOWER_OPS)
        w = (op << 25) | r.getrandbits(25)
        if op in (0x04, 0x05):  # ilw/isw: um campo só
            w = (w & ~(0xF << 21)) | (r.choice([1, 2, 4, 8]) << 21) if r.random() < 0.7 else w
        if op in (0x18, 0x1A, 0x1B, 0x1C, 0x20, 0x21, 0x24, 0x25, 0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
            w = sparse(r, w, 0xF << 21)
        if op in (0x18, 0x1A, 0x1B, 0x24, 0x25):
            w = sparse(r, w, 0x7FF)
        if op == 0x1C:
            w = sparse(r, w, (0x1F << 11) | 0x7FF)
        if op in (0x14, 0x15, 0x16, 0x17):
            w = sparse(r, w, (0x7 << 22) | (0x1F << 11))
        if op in (0x20, 0x2C, 0x2D, 0x2E, 0x2F):
            w = sparse(r, w, 0x1F << 16)
        if op in (0x20, 0x21):
            w = sparse(r, w, 0x1F << 11)
        if op == 0x24:
            w = sparse(r, w, 0x1F << 16)
        if op == 0x15:
            w = sparse(r, w, 0x1F << 16)
        return w
    if k < 0.6:
        f = r.choice(LOWER_SPECIAL)
        w = (0x40 << 25) | rnd_fields(r) | f
        return sparse(r, w, 0xF << 21)
    if k < 0.95:
        idx = r.choice(LOWER_EXT)
        w = (0x40 << 25) | (r.getrandbits(4) << 21) | (r.getrandbits(5) << 16) | (r.getrandbits(5) << 11)
        w |= ((idx >> 2) << 6) | 0x3C | (idx & 3)
        if r.random() < 0.6:
            # Zera os campos que a instrução não usa (forma canônica provável).
            zero = {
                0x39: (0x3 << 21) | (0x1F << 11), 0x3B: (0xF << 21) | (0x3FF << 11), 0x7B: (0xF << 21) | (0x3FF << 11),
                0x3C: (0x3 << 23), 0x40: 0x1F << 11, 0x41: 0x1F << 11, 0x42: (0x3 << 23) | (0x1F << 16),
                0x43: (0x3 << 23) | (0x1F << 16), 0x64: 0x1F << 11, 0x68: (0xF << 21) | (0x1F << 11),
                0x69: (0xF << 21) | (0x1F << 11), 0x6C: (0xF << 21) | (0x1F << 16),
            }.get(idx, 0)
            if 0x70 <= idx <= 0x7E and idx != 0x7B:
                zero |= 0x1F << 16
                if idx in (0x78, 0x79, 0x7A, 0x7C, 0x7D, 0x7E):
                    zero |= 0x3 << 23
                else:
                    w &= ~(0xF << 21)
                    w |= {0x70: 0xE, 0x71: 0xE, 0x72: 0xE, 0x73: 0xE, 0x74: 0xC, 0x75: 0xA, 0x76: 0xF}[idx] << 21
            w &= ~zero
        return w
    return r.getrandbits(32)


def generate(count, seed):
    r = random.Random(seed)
    pairs = []
    for _ in range(count):
        up, _ = gen_upper(r)
        lo = gen_lower(r, bool(up & 0x80000000))
        pairs.append((lo, up))
    return pairs


def objdump(pairs):
    with tempfile.TemporaryDirectory() as d:
        with open(os.path.join(d, "in.bin"), "wb") as f:
            for lo, up in pairs:
                f.write(struct.pack("<II", lo, up))
        cmd = ["docker", "run", "--rm", "-v", d + ":/w", IMAGE, "/usr/local/ps2dev/dvp/bin/dvp-objdump",
               "-D", "-b", "binary", "-m", "dvp:vu", "/w/in.bin"]
        out = subprocess.run(cmd, check=True, capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if re.match(r"^\s+[0-9a-f]+:\t", l)]
    result = {}
    for i in range(0, len(lines), 2):
        parts = lines[i].split("\t")
        addr = int(parts[0].strip().rstrip(":"), 16)
        upper = parts[2].strip() if len(parts) > 2 else ""
        lower = parts[3].strip() if len(parts) > 3 else ""
        result[addr // 8] = (upper, lower)
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["golden"])
    ap.add_argument("--count", type=int, default=12000)
    ap.add_argument("--seed", type=int, default=5900)
    ap.add_argument("--out", default=os.path.join(ROOT, "tests", "data", "vu_golden.txt"))
    a = ap.parse_args()
    pairs = generate(a.count, a.seed)
    dis = objdump(pairs)
    out = a.out
    with open(out, "w") as f:
        f.write("# Gerado por tests/scripts/vu_oracle.py a partir do dvp-objdump (-m dvp:vu).\n")
        f.write("# endereço lower upper <TAB> upper <TAB> lower\n")
        for n, (lo, up) in enumerate(pairs):
            u, l = dis[n]
            f.write(f"{n * 8:05x} {lo:08x} {up:08x}\t{u}\t{l}\n")
    print(f"{len(pairs)} pares em {out}")


if __name__ == "__main__":
    sys.exit(main())
