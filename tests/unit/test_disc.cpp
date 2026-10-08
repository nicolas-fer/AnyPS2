// Triagem de discos ("anyps2 disc"): o disco de teste do cdvd e um DVD-9
// sintético montado aqui (camada 1, IRX com e sem HLE, imagem IOPRP,
// executável principal com IRX embutidos e strings de módulos).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "anyps2/elf/elf_file.h"
#include "anyps2/runtime/iop/iso9660.h"
#include "disc.h"
#include "irx_builder.h"
#include "minitest.h"

using anyps2::disc::DeviceRef;
using anyps2::disc::Report;
using anyps2::disc::triage;
using anyps2::rt::IsoImage;

namespace {

using testutil::makeIrx;
using testutil::put16;
using testutil::put32;

constexpr std::size_t kSector = 2048;

std::vector<std::uint8_t> readHostFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    REQUIRE(f.good());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::vector<std::uint8_t> bytes(const std::string& s) {
    return std::vector<std::uint8_t>(s.begin(), s.end());
}

// Volume ISO 9660 mínimo: raiz com arquivos e subdiretórios de um nível.
struct IsoFile {
    std::string name;  // "ARQ.EXT;1"
    std::vector<std::uint8_t> data;
};
struct IsoDir {
    std::string name;
    std::vector<IsoFile> files;
    std::vector<IsoDir> dirs;  // só na raiz
};

void putBoth32(std::vector<std::uint8_t>& v, std::size_t off, std::uint32_t x) {
    put32(v, off, x);
    for (int i = 0; i < 4; ++i) v[off + 7 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x >> (8 * i));
}

void dirRecord(std::vector<std::uint8_t>& sec, std::size_t& off, const std::string& name, std::uint32_t lsn,
               std::uint32_t size, bool isDir) {
    const std::size_t len = (33 + name.size() + 1) & ~std::size_t{1};
    sec[off] = static_cast<std::uint8_t>(len);
    putBoth32(sec, off + 2, lsn);
    putBoth32(sec, off + 10, size);
    sec[off + 18] = 124;  // 2024
    sec[off + 25] = isDir ? 2 : 0;
    put16(sec, off + 28, 1);
    sec[off + 32] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), sec.begin() + static_cast<std::ptrdiff_t>(off + 33));
    off += len;
}

// Setores do volume, com LSN relativos ao início dele. `extraSpace` setores
// a mais entram no "volume space size" (sem conteúdo).
std::vector<std::uint8_t> buildVolume(const IsoDir& root, const std::string& volumeId, std::uint32_t extraSpace) {
    // 16 PVD, 17 terminador, 18 raiz, 19.. subdiretórios, depois arquivos.
    const std::uint32_t rootLsn = 18;
    std::uint32_t next = rootLsn + 1 + static_cast<std::uint32_t>(root.dirs.size());
    auto place = [&](const IsoFile& f) {
        const std::uint32_t lsn = next;
        next += static_cast<std::uint32_t>((f.data.size() + kSector - 1) / kSector);
        return lsn;
    };
    std::vector<std::vector<std::uint32_t>> fileLsn(root.dirs.size() + 1);
    for (const auto& f : root.files) fileLsn[0].push_back(place(f));
    for (std::size_t d = 0; d < root.dirs.size(); ++d) {
        for (const auto& f : root.dirs[d].files) fileLsn[d + 1].push_back(place(f));
    }
    const std::uint32_t total = next + extraSpace;
    std::vector<std::uint8_t> img(std::size_t{next} * kSector, 0);
    auto sector = [&](std::uint32_t lsn) { return img.begin() + static_cast<std::ptrdiff_t>(lsn * kSector); };

    std::vector<std::uint8_t> sec(kSector, 0);
    auto writeDir = [&](std::uint32_t lsn, std::uint32_t parent, const IsoDir& dir, std::size_t idx) {
        std::fill(sec.begin(), sec.end(), std::uint8_t{0});
        std::size_t off = 0;
        dirRecord(sec, off, std::string(1, '\0'), lsn, kSector, true);
        dirRecord(sec, off, std::string(1, '\1'), parent, kSector, true);
        for (std::size_t i = 0; i < dir.files.size(); ++i) {
            dirRecord(sec, off, dir.files[i].name, fileLsn[idx][i], static_cast<std::uint32_t>(dir.files[i].data.size()),
                      false);
        }
        if (idx == 0) {
            for (std::size_t d = 0; d < dir.dirs.size(); ++d) {
                dirRecord(sec, off, dir.dirs[d].name, rootLsn + 1 + static_cast<std::uint32_t>(d), kSector, true);
            }
        }
        std::copy(sec.begin(), sec.end(), sector(lsn));
    };
    writeDir(rootLsn, rootLsn, root, 0);
    for (std::size_t d = 0; d < root.dirs.size(); ++d) {
        writeDir(rootLsn + 1 + static_cast<std::uint32_t>(d), rootLsn, root.dirs[d], d + 1);
    }
    auto writeFiles = [&](const IsoDir& dir, std::size_t idx) {
        for (std::size_t i = 0; i < dir.files.size(); ++i) {
            std::copy(dir.files[i].data.begin(), dir.files[i].data.end(), sector(fileLsn[idx][i]));
        }
    };
    writeFiles(root, 0);
    for (std::size_t d = 0; d < root.dirs.size(); ++d) writeFiles(root.dirs[d], d + 1);

    // PVD
    std::fill(sec.begin(), sec.end(), std::uint8_t{0});
    sec[0] = 1;
    std::copy_n("CD001", 5, sec.begin() + 1);
    sec[6] = 1;
    const std::string sys = "PLAYSTATION";
    std::fill(sec.begin() + 8, sec.begin() + 72, std::uint8_t{' '});
    std::copy(sys.begin(), sys.end(), sec.begin() + 8);
    std::copy(volumeId.begin(), volumeId.end(), sec.begin() + 40);
    putBoth32(sec, 80, total);
    std::size_t off = 156;
    dirRecord(sec, off, std::string(1, '\0'), rootLsn, kSector, true);
    std::copy(sec.begin(), sec.end(), sector(16));
    // Terminador
    std::fill(sec.begin(), sec.end(), std::uint8_t{0});
    sec[0] = 255;
    std::copy_n("CD001", 5, sec.begin() + 1);
    std::copy(sec.begin(), sec.end(), sector(17));
    return img;
}

// Imagem IOPRP (ROMDIR): RESET (0 bytes), ROMDIR (a própria tabela),
// EXTINFO (0 bytes) e os módulos, cada um alinhado a 16.
std::vector<std::uint8_t> buildRomdir(const std::vector<IsoFile>& modules) {
    const std::size_t entries = 3 + modules.size() + 1;  // + terminador
    std::vector<std::uint8_t> d(entries * 16, 0);
    auto entry = [&](std::size_t i, const std::string& name, std::uint32_t size) {
        std::copy(name.begin(), name.end(), d.begin() + static_cast<std::ptrdiff_t>(i * 16));
        put32(d, i * 16 + 12, size);
    };
    entry(0, "RESET", 0);
    entry(1, "ROMDIR", static_cast<std::uint32_t>(entries * 16));
    entry(2, "EXTINFO", 0);
    for (std::size_t i = 0; i < modules.size(); ++i) {
        entry(3 + i, modules[i].name, static_cast<std::uint32_t>(modules[i].data.size()));
        d.resize((d.size() + 15) & ~std::size_t{15}, 0);
        d.insert(d.end(), modules[i].data.begin(), modules[i].data.end());
    }
    return d;
}

const DeviceRef* findRef(const Report& r, const std::string& text) {
    for (const auto& ref : r.refs) {
        if (ref.text == text) return &ref;
    }
    return nullptr;
}

bool contains(const std::string& s, const std::string& sub) {
    return s.find(sub) != std::string::npos;
}

// DVD-9 sintético; devolve o caminho e a base da camada 1.
std::string buildDvd9(std::uint32_t& layer1Base) {
    // Executável principal: um homebrew real que carrega rom0:XSIO2MAN,
    // rom0:FOOBAR e dois IRX embutidos (padman.irx com HLE, usbd.irx sem).
    // Strings extras vão anexadas ao fim do arquivo (o ELF continua válido).
    auto mainElf = readHostFile(ANYPS2_TEST_HOMEBREW_DIR "/modules/modules.elf");
    for (const char* s : {"cdrom0:\\IRX\\PADMAN.IRX;1", "cdrom0:\\IRX\\MYSND.IRX;1", "rom0:UDNL cdrom0:\\IOPRP.IMG;1",
                          "cdrom0:\\IOPRP.IMG;1", "cdrom0:\\NOPE.IRX;1", "host:dados.bin", "cdrom0:\\%s.DAT;1",
                          "cdrom0:\\DATA.BIN;1"}) {
        mainElf.push_back(0);
        mainElf.insert(mainElf.end(), s, s + std::char_traits<char>::length(s));
    }
    mainElf.push_back(0);

    IsoDir l0;
    l0.files.push_back({"SYSTEM.CNF;1", bytes("BOOT2 = cdrom0:\\MAIN.ELF;1\r\nVER = 1.02\r\nVMODE = PAL\r\n")});
    l0.files.push_back({"MAIN.ELF;1", mainElf});
    l0.files.push_back({"IOPRP.IMG;1", buildRomdir({{"CDVDMAN", makeIrx("cdvd_driver", "", 0x0203)},
                                                    {"FOOMOD", makeIrx("foo_module", "", 0x0101)},
                                                    {"SYSMEM", makeIrx("System_Memory_Manager", "", 0x0203)},
                                                    {"IOPBTCONF", bytes("CDVDMAN\nFOOMOD\n")}})});
    IsoDir irx{"IRX", {}, {}};
    irx.files.push_back({"PADMAN.IRX;1", makeIrx("", "padman", 0x0102)});
    irx.files.push_back({"MYSND.IRX;1", makeIrx("my_sound_driver", "", 0x0102)});
    l0.dirs.push_back(irx);

    IsoDir l1;
    l1.files.push_back({"OVL.ELF;1", readHostFile(ANYPS2_TEST_FIXTURES_DIR "/hello_r5900.elf")});
    l1.files.push_back({"DATA.BIN;1", std::vector<std::uint8_t>(5000, 0xAB)});

    // Como nos discos de PS2: o volume 0 declara 16 setores a mais, e a
    // camada 1 (com sua área de sistema de 16 setores) começa 16 setores
    // antes do fim declarado — o descritor dela fica exatamente no fim.
    auto img = buildVolume(l0, "SINTETICO", 16);
    layer1Base = static_cast<std::uint32_t>(img.size() / kSector);
    const auto v1 = buildVolume(l1, "SINTETICO", 0);
    img.insert(img.end(), v1.begin(), v1.end());

    const auto path = (std::filesystem::temp_directory_path() / "anyps2_test_dvd9.iso").string();
    std::ofstream o(path, std::ios::binary);
    o.write(reinterpret_cast<const char*>(img.data()), static_cast<std::streamsize>(img.size()));
    return path;
}

}  // namespace

TEST_CASE(disc, test_disc_iso) {
    const Report r = triage(ANYPS2_TEST_HOMEBREW_DIR "/cdvd/disc.iso");
    CHECK_EQ(r.volumeId, std::string("ANYPS2_TEST"));
    CHECK_EQ(r.layers, 1u);
    CHECK(!r.layer1Start.has_value());
    CHECK_EQ(r.fileCount, 4u);  // README.TXT, SYSTEM.CNF, DATA\BIG.BIN, DATA\HELLO.TXT
    CHECK_EQ(r.dirCount, 1u);
    REQUIRE(r.cnf.present);
    CHECK_EQ(r.cnf.value("BOOT2"), std::string("cdrom0:\\AP2_000.00;1"));
    CHECK_EQ(r.cnf.value("VER"), std::string("1.00"));
    CHECK_EQ(r.cnf.value("VMODE"), std::string("NTSC"));
    // O disco de teste não traz o executável do BOOT2: tem de dizer isso.
    CHECK_EQ(r.mainElfPath, std::string("\\AP2_000.00;1"));
    CHECK(contains(r.mainElfProblem, "não existe no disco"));
    CHECK(!r.mainElf.has_value());
    CHECK(r.irx.empty());
    CHECK(r.images.empty());
    CHECK(r.elfs.empty());
    REQUIRE(!r.largest.empty());
    CHECK_EQ(r.largest[0].path, std::string("\\DATA\\BIG.BIN;1"));
    CHECK_EQ(r.largest[0].size, 5000u);

    std::ostringstream out;
    anyps2::disc::print(r, out);
    CHECK(contains(out.str(), "BOOT2 = cdrom0:\\AP2_000.00;1"));
    CHECK(contains(out.str(), "PROBLEMA: o executável \\AP2_000.00;1 do BOOT2 não existe no disco"));
}

TEST_CASE(disc, iso9660_second_layer) {
    std::uint32_t base = 0;
    const std::string path = buildDvd9(base);
    {
        IsoImage iso;
        iso.open(path);
        CHECK_EQ(iso.volumeId(), std::string("SINTETICO"));
        CHECK_EQ(iso.layerCount(), 2u);
        const auto layer1 = iso.layer1Start();
        REQUIRE(layer1.has_value());
        CHECK_EQ(*layer1, base);
        // Arquivo da camada 1: LSN absoluto e conteúdo correto; não aparece
        // na camada 0 (e vice-versa).
        const auto e = iso.lookup("\\DATA.BIN;1", 1);
        REQUIRE(e.has_value());
        CHECK_EQ(e->layer, 1u);
        CHECK(e->lsn > base);
        std::uint8_t b[3] = {};
        CHECK(iso.readBytes(e->lsn, 4997, 3, b));
        CHECK_EQ(b[0], 0xAB);
        CHECK_EQ(b[2], 0xAB);
        CHECK(!iso.lookup("DATA.BIN", 0).has_value());
        CHECK(!iso.lookup("SYSTEM.CNF", 1).has_value());
        CHECK(iso.lookup("IRX/MYSND.IRX").has_value());
    }
    std::filesystem::remove(path);
}

TEST_CASE(disc, synthetic_dvd9) {
    std::uint32_t base = 0;
    const std::string path = buildDvd9(base);
    const Report r = triage(path);
    std::ostringstream out;
    anyps2::disc::print(r, out);
    std::filesystem::remove(path);

    CHECK_EQ(r.layers, 2u);
    REQUIRE(r.layer1Start.has_value());
    CHECK_EQ(*r.layer1Start, base);
    CHECK_EQ(r.fileCount, 7u);
    CHECK_EQ(r.dirCount, 1u);
    CHECK_EQ(r.cnf.value("VMODE"), std::string("PAL"));

    // Executável principal
    CHECK_EQ(r.mainElfPath, std::string("\\MAIN.ELF;1"));
    CHECK(r.mainElfProblem.empty());
    REQUIRE(r.mainElf.has_value());
    const auto ref = anyps2::elf::ElfFile::loadFromFile(ANYPS2_TEST_HOMEBREW_DIR "/modules/modules.elf");
    CHECK_EQ(r.mainElf->entry, ref.entry());
    CHECK_EQ(r.mainElf->segments.size(), ref.segments().size());
    CHECK(r.mainElf->r5900);

    // IRX embutidos no executável
    REQUIRE(r.embedded.size() == 2u);
    std::vector<std::string> embedded;
    for (const auto& x : r.embedded) embedded.push_back(x.name + "=" + (x.hle.empty() ? "-" : x.hle));
    std::sort(embedded.begin(), embedded.end());
    CHECK_EQ(embedded[1], std::string("padman=padman"));
    CHECK(contains(embedded[0], "=-"));  // usbd.irx: sem HLE

    // Strings de módulos
    const DeviceRef* sio = findRef(r, "rom0:XSIO2MAN");
    REQUIRE(sio != nullptr);
    CHECK(sio->module);
    CHECK_EQ(sio->hle, std::string("sio2man"));
    const DeviceRef* foobar = findRef(r, "rom0:FOOBAR");
    REQUIRE(foobar != nullptr);
    CHECK(foobar->hle.empty());
    CHECK(contains(foobar->status, "SEM HLE"));
    const DeviceRef* pad = findRef(r, "cdrom0:\\IRX\\PADMAN.IRX;1");
    REQUIRE(pad != nullptr);
    CHECK_EQ(pad->hle, std::string("padman"));
    const DeviceRef* snd = findRef(r, "cdrom0:\\IRX\\MYSND.IRX;1");
    REQUIRE(snd != nullptr);
    CHECK(snd->module);
    CHECK(contains(snd->status, "\"my_sound_driver\" v1.2 — SEM HLE"));
    const DeviceRef* udnl = findRef(r, "rom0:UDNL cdrom0:\\IOPRP.IMG;1");
    REQUIRE(udnl != nullptr);
    CHECK_EQ(udnl->device, std::string("rom0"));
    CHECK(contains(udnl->status, "IOPRP"));
    const DeviceRef* img = findRef(r, "cdrom0:\\IOPRP.IMG;1");
    REQUIRE(img != nullptr);
    CHECK(contains(img->status, "imagem IOPRP do disco (4 módulos)"));
    const DeviceRef* nope = findRef(r, "cdrom0:\\NOPE.IRX;1");
    REQUIRE(nope != nullptr);
    CHECK(contains(nope->status, "não existe no disco"));
    // Só o nome do IRX (o programa monta o caminho): casa com o arquivo do disco.
    const DeviceRef* bare = findRef(r, "padman.irx");
    REQUIRE(bare != nullptr);
    CHECK(bare->module);
    CHECK(bare->device.empty());
    CHECK_EQ(bare->hle, std::string("padman"));
    const DeviceRef* usbd = findRef(r, "usbd.irx");
    REQUIRE(usbd != nullptr);
    CHECK(contains(usbd->status, "nenhum arquivo com esse nome"));
    const DeviceRef* host = findRef(r, "host:dados.bin");
    REQUIRE(host != nullptr);
    CHECK(!host->module);
    CHECK_EQ(host->device, std::string("host"));
    const DeviceRef* fmt = findRef(r, "cdrom0:\\%s.DAT;1");
    REQUIRE(fmt != nullptr);
    CHECK(contains(fmt->status, "tempo de execução"));
    const DeviceRef* data = findRef(r, "cdrom0:\\DATA.BIN;1");
    REQUIRE(data != nullptr);
    CHECK(contains(data->status, "5000 bytes) [camada 1]"));

    // IRX do disco
    REQUIRE(r.irx.size() == 2u);
    for (const auto& x : r.irx) {
        if (x.name == "padman") {
            CHECK_EQ(x.hle, std::string("padman"));
            CHECK_EQ(x.version, 0x0102);
        } else {
            CHECK_EQ(x.name, std::string("my_sound_driver"));
            CHECK(x.hle.empty());
            CHECK_EQ(x.file.path, std::string("\\IRX\\MYSND.IRX;1"));
        }
    }

    // Imagem IOPRP
    REQUIRE(r.images.size() == 1u);
    const auto& mods = r.images[0].modules;
    REQUIRE(mods.size() == 4u);
    CHECK_EQ(mods[0].romName, std::string("CDVDMAN"));
    CHECK_EQ(mods[0].irxName, std::string("cdvd_driver"));
    CHECK_EQ(mods[0].version, 0x0203);
    CHECK_EQ(mods[0].hle, std::string("cdvdman"));
    CHECK_EQ(mods[1].irxName, std::string("foo_module"));
    CHECK(mods[1].hle.empty());
    CHECK(!mods[1].kernel);
    CHECK(mods[2].hle.empty());
    CHECK(mods[2].kernel);  // núcleo do IOP: não conta como faltante no resumo
    CHECK(!mods[3].irx);
    CHECK(contains(r.images[0].bootConfig, "FOOMOD"));

    // Outro ELF (overlay) na camada 1
    REQUIRE(r.elfs.size() == 1u);
    CHECK_EQ(r.elfs[0].file.path, std::string("\\OVL.ELF;1"));
    CHECK_EQ(r.elfs[0].file.layer, 1u);
    CHECK(r.elfs[0].r5900);

    const std::string text = out.str();
    CHECK(contains(text, "DVD de camada dupla (camada 1 a partir do setor " + std::to_string(base) + ")"));
    CHECK(contains(text, "\\OVL.ELF;1 [camada 1]"));
    CHECK(contains(text, "Resumo: 1 de 2 IRX do disco sem HLE; 1 módulo(s) da ROM referenciado(s) sem HLE; "
                         "1 módulo(s) de imagens IOPRP sem HLE; 1 outro(s) ELF(s) no disco."));
}
