#!/usr/bin/env python3
"""Gera disc.iso, a imagem ISO 9660 mínima usada pelo teste cdvd.

Conteúdo nosso, datas fixas: a saída é sempre a mesma. Uso:
    python3 make_iso.py [saída.iso]
"""
import struct
import sys

SECTOR = 2048
DATE7 = bytes([124, 5, 17, 12, 34, 56, 0])  # 2024-05-17 12:34:56 GMT
DATE17 = b"2024051712345600" + b"\x00"


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def dir_record(name, lba, size, is_dir):
    n = len(name)
    rec = bytes([0, 0]) + both32(lba) + both32(size) + DATE7 + bytes([2 if is_dir else 0, 0, 0]) + both16(1)
    rec += bytes([n]) + name
    if n % 2 == 0:
        rec += b"\x00"
    rec = bytes([len(rec)]) + rec[1:]
    return rec


def pad_text(s, n):
    return s.encode("ascii").ljust(n, b" ")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "disc.iso"
    files_root = [
        (b"README.TXT;1", b"Disco de teste do AnyPS2.\nLido via cdrom0: pelo fileio.\n"),
        (b"SYSTEM.CNF;1", b"BOOT2 = cdrom0:\\AP2_000.00;1\r\nVER = 1.00\r\nVMODE = NTSC\r\n"),
    ]
    big = bytes((i * 7 + (i >> 8)) & 0xFF for i in range(5000))
    files_data = [
        (b"BIG.BIN;1", big),
        (b"HELLO.TXT;1", b"Ola do setor do disco!\n"),
    ]
    # Layout: 16 PVD, 17 terminador, 18 path table L, 19 path table M,
    # 20 raiz, 21 DATA, 22.. arquivos.
    root_lba, data_lba = 20, 21
    lba = 22
    placed_root, placed_data = [], []
    for name, content in files_root:
        placed_root.append((name, lba, content))
        lba += (len(content) + SECTOR - 1) // SECTOR
    for name, content in files_data:
        placed_data.append((name, lba, content))
        lba += (len(content) + SECTOR - 1) // SECTOR
    total = lba

    def dir_sector(self_lba, parent_lba, entries):
        d = dir_record(b"\x00", self_lba, SECTOR, True) + dir_record(b"\x01", parent_lba, SECTOR, True)
        for name, l, size, is_dir in entries:
            d += dir_record(name, l, size, is_dir)
        return d.ljust(SECTOR, b"\x00")

    root_entries = [(b"DATA", data_lba, SECTOR, True)] + [(n, l, len(c), False) for n, l, c in placed_root]
    root_entries.sort(key=lambda e: e[0])
    data_entries = [(n, l, len(c), False) for n, l, c in placed_data]

    def path_table(big_endian):
        p32 = ">I" if big_endian else "<I"
        p16 = ">H" if big_endian else "<H"
        t = bytes([1, 0]) + struct.pack(p32, root_lba) + struct.pack(p16, 1) + b"\x00\x00"
        t += bytes([4, 0]) + struct.pack(p32, data_lba) + struct.pack(p16, 1) + b"DATA"
        return t

    pt = path_table(False)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[8:40] = pad_text("PLAYSTATION", 32)
    pvd[40:72] = pad_text("ANYPS2_TEST", 32)
    pvd[80:88] = both32(total)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(SECTOR)
    pvd[132:140] = both32(len(pt))
    pvd[140:144] = struct.pack("<I", 18)
    pvd[148:152] = struct.pack(">I", 19)
    pvd[156:190] = dir_record(b"\x00", root_lba, SECTOR, True)
    for off, n in ((190, 128), (318, 128), (446, 128), (574, 128), (702, 37), (739, 37), (776, 37)):
        pvd[off:off + n] = b" " * n
    pvd[318:318 + 6] = b"ANYPS2"
    for off in (813, 830):
        pvd[off:off + 17] = DATE17
    for off in (847, 864):
        pvd[off:off + 17] = b"0" * 16 + b"\x00"
    pvd[881] = 1
    term = bytearray(SECTOR)
    term[0] = 255
    term[1:6] = b"CD001"
    term[6] = 1

    img = bytearray(SECTOR * 16)
    img += pvd + term
    img += pt.ljust(SECTOR, b"\x00") + path_table(True).ljust(SECTOR, b"\x00")
    img += dir_sector(root_lba, root_lba, root_entries)
    img += dir_sector(data_lba, root_lba, data_entries)
    for _, _, content in placed_root + placed_data:
        img += content.ljust((len(content) + SECTOR - 1) // SECTOR * SECTOR, b"\x00")
    assert len(img) == total * SECTOR
    with open(out, "wb") as f:
        f.write(img)


if __name__ == "__main__":
    main()
