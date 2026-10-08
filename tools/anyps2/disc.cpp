// "anyps2 disc": triagem de um disco de PS2. Percorre o ISO 9660 (as duas
// camadas de um DVD-9), lê o SYSTEM.CNF, analisa o executável principal e
// classifica cada IRX/imagem de módulos/ELF do disco contra a tabela de
// módulos com HLE do runtime.

#include "disc.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "anyps2/common/error.h"
#include "anyps2/elf/elf_file.h"
#include "anyps2/runtime/iop/irx.h"
#include "anyps2/runtime/iop/iso9660.h"

namespace anyps2::disc {

namespace {

using rt::IsoImage;

constexpr std::uint32_t kMaxRead = 64u << 20;  // nenhum ELF/IRX/IOPRP real chega perto
constexpr std::uint16_t kEtSceEeRelExec = 0xFF91;  // ERX: módulo relocável do EE (SCE)

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}

// "ARQ.IRX;1" → "ARQ.IRX"
std::string withoutVersion(const std::string& s) {
    return s.substr(0, s.find(';'));
}

bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string versionText(std::uint16_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%u.%u", v >> 8, v & 0xFFu);
    return buf;
}

std::string layerTag(unsigned layer) {
    return layer == 1 ? " [camada 1]" : "";
}

// Módulos do núcleo do IOP (os que a ROM carrega no boot e que imagens IOPRP
// substituem). O EE nunca fala com eles diretamente: o IOP em HLE os cobre
// como um todo (SIF, RPC, loadfile, heap). Só passam a contar se algum
// driver do IOP tiver de ser executado de verdade.
bool isIopKernelModule(const std::string& romName) {
    static const char* const kKernel[] = {
        "SYSMEM",   "LOADCORE", "EXCEPMAN", "INTRMANP", "INTRMANI", "SSBUSC",  "DMACMAN", "TIMRMANP",
        "TIMRMANI", "TIMEMANI", "SYSCLIB",  "HEAPLIB",  "THREADMAN", "VBLANK", "IOMAN",   "STDIO",
        "SIFMAN",   "SIFCMD",   "SIFINIT",  "LOADFILE", "MODLOAD",  "ROMDRV",  "REBOOT",  "SECRMAN",
        "EESYNC",
    };
    for (const char* k : kKernel) {
        if (romName == k) return true;
    }
    return false;
}

std::string hleOf(const char* p) {
    return p ? std::string(p) : std::string();
}

std::optional<std::vector<std::uint8_t>> readPart(IsoImage& iso, const DiscFile& f, std::uint32_t max) {
    std::vector<std::uint8_t> d(std::min(f.size, max));
    if (!d.empty() && !iso.readBytes(f.lsn, 0, static_cast<std::uint32_t>(d.size()), d.data())) {
        return std::nullopt;
    }
    return d;
}

void walk(IsoImage& iso, const IsoImage::Entry& dir, const std::string& prefix, std::set<std::uint32_t>& seen,
          Report& r, std::vector<DiscFile>& files) {
    if (!seen.insert(dir.lsn).second) return;  // diretório que aponta para um ancestral
    for (const auto& e : iso.list(dir)) {
        const std::string path = prefix + "\\" + e.name;
        if (e.isDir) {
            ++r.dirCount;
            walk(iso, e, path, seen, r, files);
        } else {
            files.push_back({path, e.layer, e.lsn, e.size});
            ++r.fileCount;
            r.totalBytes += e.size;
        }
    }
}

SystemCnf parseSystemCnf(const std::vector<std::uint8_t>& d) {
    SystemCnf cnf;
    cnf.present = true;
    std::string text(d.begin(), d.end());
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find_first_of("\r\n", pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = upper(trim(line.substr(0, eq)));
        if (!key.empty()) cnf.fields.emplace_back(key, trim(line.substr(eq + 1)));
    }
    return cnf;
}

// ROMDIR: entradas de 16 bytes {char nome[10]; u16 extinfo; u32 tamanho} a
// partir do offset 0 (nas imagens IOPRP o RESET tem tamanho 0, então a
// própria tabela é o arquivo ROMDIR); cada arquivo começa alinhado a 16.
std::optional<IopImage> parseRomdir(const std::vector<std::uint8_t>& d) {
    if (d.size() < 48 || std::memcmp(d.data(), "RESET\0", 6) != 0) return std::nullopt;
    IopImage img;
    bool sawRomdir = false;
    std::size_t off = 0, tableEnd = d.size();
    for (std::size_t e = 0; e + 16 <= tableEnd; e += 16) {
        const std::uint8_t* rec = d.data() + e;
        if (rec[0] == 0) break;
        const std::string name(reinterpret_cast<const char*>(rec), strnlen(reinterpret_cast<const char*>(rec), 10));
        const std::uint32_t size = le32(rec + 12);
        if (off + size > d.size()) return std::nullopt;
        if (name == "ROMDIR") {
            sawRomdir = true;
            if (off == 0) tableEnd = size;  // a tabela é o próprio arquivo ROMDIR
        } else if (name != "RESET" && name != "EXTINFO") {
            ImageModule m;
            m.romName = name;
            m.size = size;
            if (const auto info = rt::parseIrx(d.data() + off, size)) {
                m.irx = true;
                m.irxName = info->name;
                m.version = info->version;
                m.hle = hleOf(rt::hleModuleForIrx(info->name));
            }
            if (m.hle.empty()) m.hle = hleOf(rt::hleModuleForRom(name));
            m.kernel = m.hle.empty() && isIopKernelModule(name);
            if (name == "IOPBTCONF") img.bootConfig.assign(d.begin() + static_cast<std::ptrdiff_t>(off),
                                                           d.begin() + static_cast<std::ptrdiff_t>(off + size));
            img.modules.push_back(std::move(m));
        }
        off += (std::size_t{size} + 15) & ~std::size_t{15};
    }
    if (!sawRomdir) return std::nullopt;
    return img;
}

struct Device {
    const char* prefix;
    const char* name;
};
// cdrom0: antes de rom0: (um contém o outro).
constexpr Device kDevices[] = {
    {"cdrom0:", "cdrom0"}, {"cdrom:", "cdrom0"}, {"host0:", "host"}, {"host:", "host"}, {"rom0:", "rom0"},
    {"rom1:", "rom1"},     {"rom2:", "rom2"},    {"mc0:", "mc0"},    {"mc1:", "mc1"},   {"hdd0:", "hdd0"},
    {"pfs0:", "pfs0"},
};

bool matchesAt(const std::string& s, std::size_t i, const char* prefix) {
    const std::size_t n = std::strlen(prefix);
    if (i + n > s.size()) return false;
    for (std::size_t k = 0; k < n; ++k) {
        if (std::tolower(static_cast<unsigned char>(s[i + k])) != prefix[k]) return false;
    }
    return true;
}

// Para "cdrom0:\IRX\X.IRX;1" ou "rom0:XPADMAN" (sem cdrom0:/rom0:).
const DiscIrx* irxAt(const Report& r, std::uint32_t lsn) {
    for (const auto& x : r.irx) {
        if (x.file.lsn == lsn) return &x;
    }
    return nullptr;
}
const IopImage* imageAt(const Report& r, std::uint32_t lsn) {
    for (const auto& x : r.images) {
        if (x.file.lsn == lsn) return &x;
    }
    return nullptr;
}

void describeRef(IsoImage& iso, const Report& r, DeviceRef& ref) {
    const std::string target = upper(withoutVersion(ref.target));
    ref.module = ref.device.rfind("rom", 0) == 0 || endsWith(target, ".IRX") || endsWith(target, ".IMG");
    if (ref.device.empty()) {  // só o nome do IRX: procura um arquivo com esse nome no disco
        for (const auto& x : r.irx) {
            const auto slash = x.file.path.find_last_of('\\');
            if (upper(withoutVersion(x.file.path.substr(slash + 1))) != target) continue;
            ref.hle = x.hle;
            ref.status = "nome de IRX (caminho montado em tempo de execução) = " + x.file.path + " \"" + x.name +
                         "\" v" + versionText(x.version) + (x.hle.empty() ? " — SEM HLE" : " — HLE: " + x.hle);
            return;
        }
        ref.status = "nome de IRX (caminho montado em tempo de execução) — nenhum arquivo com esse nome no disco";
        return;
    }
    if (ref.device.rfind("rom", 0) == 0) {
        const std::string rom = rt::romModuleName(ref.device + ":" + ref.target);
        ref.hle = hleOf(rt::hleModuleForRom(rom));
        if (rom == "UDNL") {
            ref.status = "reboot do IOP com uma imagem de módulos substitutos (IOPRP); o HLE aceita o reboot "
                         "e ignora a imagem — os módulos dela contam pelo HLE de cada um";
        } else if (!ref.hle.empty()) {
            ref.status = "módulo da ROM do console — HLE: " + ref.hle;
        } else {
            ref.status = "módulo da ROM do console — SEM HLE";
        }
        return;
    }
    if (ref.device == "cdrom0") {
        if (ref.target.find('%') != std::string::npos) {
            ref.status = "caminho montado em tempo de execução (formato printf)";
            return;
        }
        std::optional<IsoImage::Entry> e;
        for (unsigned layer = 0; layer < iso.layerCount() && !e; ++layer) e = iso.lookup(ref.target, layer);
        if (!e) {
            ref.status = "não existe no disco";
            return;
        }
        const std::string where = layerTag(e->layer);
        if (const DiscIrx* x = irxAt(r, e->lsn)) {
            ref.hle = x->hle;
            ref.status = "IRX do disco \"" + x->name + "\" v" + versionText(x->version) +
                         (x->hle.empty() ? " — SEM HLE" : " — HLE: " + x->hle) + where;
        } else if (const IopImage* img = imageAt(r, e->lsn)) {
            ref.status = "imagem IOPRP do disco (" + std::to_string(img->modules.size()) + " módulos)" + where;
        } else if (e->isDir) {
            ref.status = "diretório do disco" + where;
        } else {
            ref.status = "arquivo do disco (" + std::to_string(e->size) + " bytes)" + where;
        }
        return;
    }
    if (ref.device == "host") {
        ref.status = "PC do kit de desenvolvimento (fora do disco); no AnyPS2 é relativo a ANYPS2_HOST_DIR";
    } else if (ref.device == "mc0" || ref.device == "mc1") {
        ref.status = "memory card — pelo libmc funciona; por fopen/fileio não é suportado";
    } else {
        ref.status = "HDD — não suportado";
    }
}

// "sio2man.irx" (só o nome do arquivo, sem espaços nem dispositivo): o
// programa monta o caminho em tempo de execução.
bool isBareIrxName(const std::string& s) {
    const std::string u = upper(withoutVersion(s));
    if (!endsWith(u, ".IRX") || u.size() <= 4) return false;
    for (std::size_t i = 0; i + 4 < u.size(); ++i) {
        const auto c = static_cast<unsigned char>(u[i]);
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    return true;
}

// Strings imprimíveis do ELF com um caminho de dispositivo (ou só o nome de
// um IRX).
std::vector<DeviceRef> findDeviceRefs(const std::vector<std::uint8_t>& d) {
    std::vector<DeviceRef> refs;
    std::size_t i = 0;
    while (i < d.size()) {
        if (d[i] < 0x20 || d[i] > 0x7E) {
            ++i;
            continue;
        }
        const std::size_t start = i;
        while (i < d.size() && d[i] >= 0x20 && d[i] <= 0x7E) ++i;
        if (i - start < 4) continue;
        const std::string s(d.begin() + static_cast<std::ptrdiff_t>(start), d.begin() + static_cast<std::ptrdiff_t>(i));
        if (isBareIrxName(s)) {
            DeviceRef ref;
            ref.offset = static_cast<std::uint32_t>(start);
            ref.text = s;
            ref.target = s;
            refs.push_back(std::move(ref));
            continue;
        }
        for (std::size_t k = 0; k < s.size(); ++k) {
            if (k > 0 && std::isalnum(static_cast<unsigned char>(s[k - 1]))) continue;
            const Device* dev = nullptr;
            for (const Device& cand : kDevices) {
                if (matchesAt(s, k, cand.prefix)) {
                    dev = &cand;
                    break;
                }
            }
            if (!dev) continue;
            DeviceRef ref;
            ref.offset = static_cast<std::uint32_t>(start);
            ref.text = s;
            ref.device = dev->name;
            // O caminho termina no primeiro caractere que não cabe nele
            // ("rom0:XSIO2MAN: %s", "carregando rom0:FOOBAR...").
            const std::size_t t = k + std::strlen(dev->prefix);
            const std::size_t stop = s.find_first_of(" :\"',()", t);
            ref.target = s.substr(t, stop == std::string::npos ? std::string::npos : stop - t);
            while (!ref.target.empty() && ref.target.back() == '.') ref.target.pop_back();
            refs.push_back(std::move(ref));
            break;  // uma referência por string (a primeira)
        }
    }
    return refs;
}

std::vector<EmbeddedIrx> findEmbeddedIrx(const std::vector<std::uint8_t>& d) {
    std::vector<EmbeddedIrx> out;
    for (std::size_t off = 1; off + 52 <= d.size(); ++off) {
        if (d[off] != 0x7F || d[off + 1] != 'E' || d[off + 2] != 'L' || d[off + 3] != 'F') continue;
        if (const auto info = rt::parseIrx(d.data() + off, d.size() - off)) {
            out.push_back({static_cast<std::uint32_t>(off), info->name, info->version,
                           hleOf(rt::hleModuleForIrx(info->name))});
        }
    }
    return out;
}

void analyzeMainElf(IsoImage& iso, Report& r) {
    const std::string boot2 = r.cnf.value("BOOT2");
    if (!r.cnf.present) {
        r.mainElfProblem = "o disco não tem SYSTEM.CNF na raiz (não é um disco de jogo de PS2)";
        return;
    }
    if (boot2.empty()) {
        r.mainElfProblem = r.cnf.value("BOOT").empty()
                               ? "o SYSTEM.CNF não tem BOOT2"
                               : "o SYSTEM.CNF tem BOOT e não BOOT2: é um disco de PlayStation 1";
        return;
    }
    const auto colon = boot2.find(':');
    std::string path = colon == std::string::npos ? boot2 : boot2.substr(colon + 1);
    r.mainElfPath = path;
    if (colon == std::string::npos || upper(boot2.substr(0, colon)) != "CDROM0") {
        r.mainElfProblem = "BOOT2 não aponta para cdrom0: (" + boot2 + ")";
        return;
    }
    const auto e = iso.lookup(path);
    if (!e || e->isDir) {
        r.mainElfProblem = "o executável " + path + " do BOOT2 não existe no disco";
        return;
    }
    const DiscFile file{path, 0, e->lsn, e->size};
    const auto data = readPart(iso, file, kMaxRead);
    if (!data || data->size() != e->size) {
        r.mainElfProblem = "o executável " + path + " passa do fim da imagem (dump incompleto?)";
        return;
    }
    try {
        const auto f = elf::ElfFile::loadFromMemory(*data, path);
        ElfSummary s;
        s.fileSize = data->size();
        s.type = f.type();
        s.entry = f.entry();
        s.eflags = f.flags();
        s.r5900 = f.isR5900();
        s.loadBase = f.loadBase();
        s.loadEnd = f.loadEnd();
        for (const auto& seg : f.segments()) {
            s.segments.push_back({seg.type, seg.offset, seg.vaddr, seg.filesz, seg.memsz, seg.flags});
        }
        s.sections = f.sections().size();
        s.symbols = f.symbols().size();
        for (const auto& sym : f.symbols()) s.functions += sym.isFunction() ? 1u : 0u;
        r.mainElf = s;
    } catch (const anyps2::Error& ex) {
        r.mainElfProblem = std::string("o executável não é um ELF válido: ") + ex.what();
    }
    r.refs = findDeviceRefs(*data);
    for (auto& ref : r.refs) describeRef(iso, r, ref);
    r.embedded = findEmbeddedIrx(*data);
}

const char* segmentTypeName(std::uint32_t type) {
    switch (type) {
        case elf::PT_NULL: return "NULL";
        case elf::PT_LOAD: return "LOAD";
        case elf::PT_SCE_IOPMOD: return "SCE_IOPMOD";
        default: return nullptr;
    }
}

std::string mb(std::uint64_t bytes) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

}  // namespace

std::string SystemCnf::value(const std::string& key) const {
    for (const auto& [k, v] : fields) {
        if (k == key) return v;
    }
    return {};
}

Report triage(const std::string& isoPath) {
    IsoImage iso;
    iso.open(isoPath);
    Report r;
    r.isoPath = isoPath;
    r.volumeId = iso.volumeId();
    r.sectors = iso.sectorCount();
    r.layers = iso.layerCount();
    r.layer1Start = iso.layer1Start();

    std::vector<DiscFile> files;
    std::set<std::uint32_t> seen;
    for (unsigned layer = 0; layer < iso.layerCount(); ++layer) walk(iso, iso.root(layer), "", seen, r, files);

    r.largest = files;
    std::stable_sort(r.largest.begin(), r.largest.end(),
                     [](const DiscFile& a, const DiscFile& b) { return a.size > b.size; });
    if (r.largest.size() > 10) r.largest.resize(10);

    if (const auto cnf = iso.lookup("SYSTEM.CNF"); cnf && !cnf->isDir) {
        if (const auto d = readPart(iso, {"\\SYSTEM.CNF", 0, cnf->lsn, cnf->size}, 64 * 1024)) {
            r.cnf = parseSystemCnf(*d);
        }
    }
    std::optional<std::uint32_t> mainLsn;
    if (const std::string boot2 = r.cnf.value("BOOT2"); !boot2.empty()) {
        const auto colon = boot2.find(':');
        if (const auto e = iso.lookup(colon == std::string::npos ? boot2 : boot2.substr(colon + 1))) {
            mainLsn = e->lsn;
        }
    }

    // Classifica os arquivos pelo conteúdo (não pela extensão).
    for (const auto& f : files) {
        if (f.size < 16 || (mainLsn && f.lsn == *mainLsn)) continue;
        std::uint8_t head[64] = {};
        const std::uint32_t n = std::min<std::uint32_t>(f.size, sizeof(head));
        if (!iso.readBytes(f.lsn, 0, n, head)) continue;
        const bool isElf = head[0] == 0x7F && head[1] == 'E' && head[2] == 'L' && head[3] == 'F';
        const bool irxName = endsWith(upper(withoutVersion(f.path)), ".IRX");
        if ((isElf && n >= 52 && le16(head + 16) == elf::ET_SCE_IOPRELEXEC) || (isElf && irxName)) {
            DiscIrx x;
            x.file = f;
            if (const auto d = readPart(iso, f, kMaxRead)) {
                if (const auto info = rt::parseIrx(d->data(), d->size())) {
                    x.name = info->name;
                    x.version = info->version;
                    x.hle = hleOf(rt::hleModuleForIrx(info->name));
                }
            }
            r.irx.push_back(std::move(x));
        } else if (isElf && n >= 52) {
            r.elfs.push_back({f, le16(head + 16), (le32(head + 36) & elf::EF_MIPS_MACH) == elf::E_MIPS_MACH_5900});
        } else if (std::memcmp(head, "RESET\0", 6) == 0) {
            if (const auto d = readPart(iso, f, kMaxRead)) {
                if (auto img = parseRomdir(*d)) {
                    img->file = f;
                    r.images.push_back(std::move(*img));
                }
            }
        }
    }

    analyzeMainElf(iso, r);
    return r;
}

void print(const Report& r, std::ostream& out) {
    char line[512];
    out << "Disco: " << r.isoPath << "\n";
    out << "  volume:   " << (r.volumeId.empty() ? "(sem nome)" : r.volumeId) << ", " << r.sectors << " setores ("
        << mb(std::uint64_t{r.sectors} * IsoImage::kSectorSize) << ")";
    if (r.layer1Start) out << ", DVD de camada dupla (camada 1 a partir do setor " << *r.layer1Start << ")";
    out << "\n  conteúdo: " << r.fileCount << " arquivos em " << r.dirCount << " diretórios, " << mb(r.totalBytes)
        << "\n";

    out << "\nSYSTEM.CNF";
    if (!r.cnf.present) {
        out << ": ausente\n";
    } else {
        out << "\n";
        for (const auto& [k, v] : r.cnf.fields) out << "  " << k << " = " << v << "\n";
    }

    out << "\nExecutável principal";
    if (!r.mainElfPath.empty()) out << ": " << r.mainElfPath;
    out << "\n";
    if (!r.mainElfProblem.empty()) out << "  PROBLEMA: " << r.mainElfProblem << "\n";
    if (r.mainElf) {
        const ElfSummary& s = *r.mainElf;
        out << "  " << s.fileSize << " bytes, tipo " << elf::ElfFile::typeName(s.type) << ", "
            << (s.r5900 ? "R5900 (Emotion Engine)" : "sem marca de R5900 em e_flags") << ", entrada "
            << hex(s.entry) << ", carregado em " << hex(s.loadBase) << ".." << hex(s.loadEnd) << "\n";
        out << "  segmentos (" << s.segments.size() << "):\n";
        out << "    Tipo        Offset      VAddr       FileSz      MemSz       Flags\n";
        for (const auto& g : s.segments) {
            const char* tn = segmentTypeName(g.type);
            std::snprintf(line, sizeof(line), "    %-11s %s  %s  %s  %s  %c%c%c\n",
                          tn ? tn : hex(g.type).c_str(), hex(g.offset).c_str(), hex(g.vaddr).c_str(),
                          hex(g.filesz).c_str(), hex(g.memsz).c_str(), (g.flags & elf::PF_R) ? 'R' : '-',
                          (g.flags & elf::PF_W) ? 'W' : '-', (g.flags & elf::PF_X) ? 'X' : '-');
            out << line;
        }
        out << "  seções: " << s.sections << ", símbolos: " << s.symbols << " (" << s.functions << " funções)"
            << (s.symbols == 0 ? " — sem tabela de símbolos" : "") << "\n";
    }

    auto printRefs = [&](bool modules, const char* title) {
        std::size_t n = 0;
        for (const auto& ref : r.refs) n += ref.module == modules ? 1u : 0u;
        out << "\n" << title << " (" << n << ")" << (n ? ":" : "") << "\n";
        for (const auto& ref : r.refs) {
            if (ref.module != modules) continue;
            out << "  " << hex(ref.offset, 6) << "  \"" << ref.text << "\"\n      → " << ref.status << "\n";
        }
    };
    if (r.mainElf || !r.refs.empty()) {
        printRefs(true, "Módulos do IOP referenciados pelo executável principal");
        printRefs(false, "Outros caminhos de dispositivo no executável principal");
        out << "\nIRX embutidos no executável principal (" << r.embedded.size() << ")" << (r.embedded.empty() ? "" : ":")
            << "\n";
        for (const auto& x : r.embedded) {
            out << "  offset " << hex(x.offset, 6) << "  \"" << x.name << "\" v" << versionText(x.version) << "  "
                << (x.hle.empty() ? "SEM HLE" : "HLE: " + x.hle) << "\n";
        }
    }

    out << "\nIRX no disco (" << r.irx.size() << ")" << (r.irx.empty() ? "" : ":") << "\n";
    for (const auto& x : r.irx) {
        std::snprintf(line, sizeof(line), "  %-36s %8u bytes  ", (x.file.path + layerTag(x.file.layer)).c_str(),
                      x.file.size);
        out << line;
        if (x.name.empty() && x.version == 0) {
            out << "cabeçalho .iopmod não reconhecido — SEM HLE\n";
        } else {
            out << "\"" << x.name << "\" v" << versionText(x.version) << "  "
                << (x.hle.empty() ? "SEM HLE" : "HLE: " + x.hle) << "\n";
        }
    }

    out << "\nImagens de módulos do IOP (IOPRP/ROMDIR) (" << r.images.size() << ")" << (r.images.empty() ? "" : ":")
        << "\n";
    for (const auto& img : r.images) {
        out << "  " << img.file.path << layerTag(img.file.layer) << ": " << img.modules.size() << " módulos\n";
        for (const auto& m : img.modules) {
            std::snprintf(line, sizeof(line), "    %-10s %7u bytes  ", m.romName.c_str(), m.size);
            out << line;
            if (m.irx) out << "\"" << m.irxName << "\" v" << versionText(m.version) << "  ";
            else out << "(não é IRX)  ";
            if (!m.hle.empty()) out << "HLE: " << m.hle;
            else if (m.kernel) out << "núcleo do IOP (coberto pelo IOP em HLE)";
            else if (m.irx) out << "SEM HLE";
            out << "\n";
        }
        if (!img.bootConfig.empty()) {
            out << "    IOPBTCONF:";
            std::string cfg = img.bootConfig;
            std::replace(cfg.begin(), cfg.end(), '\r', ' ');
            std::replace(cfg.begin(), cfg.end(), '\n', ' ');
            out << " " << trim(cfg) << "\n";
        }
    }

    out << "\nOutros ELFs no disco (" << r.elfs.size() << ")" << (r.elfs.empty() ? "" : ":") << "\n";
    for (const auto& e : r.elfs) {
        out << "  " << e.file.path << layerTag(e.file.layer) << "  " << e.file.size << " bytes, ";
        if (e.type == kEtSceEeRelExec) out << "ERX (módulo relocável do EE, carregado em tempo de execução)";
        else out << "tipo " << elf::ElfFile::typeName(e.type) << " (" << hex(e.type, 4) << ")";
        out << (e.r5900 ? ", R5900" : "") << "\n";
    }

    out << "\nMaiores arquivos:\n";
    for (const auto& f : r.largest) {
        std::snprintf(line, sizeof(line), "  %-36s %12u bytes (%s)\n", (f.path + layerTag(f.layer)).c_str(), f.size,
                      mb(f.size).c_str());
        out << line;
    }

    std::size_t irxNoHle = 0, imgNoHle = 0;
    for (const auto& x : r.irx) irxNoHle += x.hle.empty() ? 1u : 0u;
    std::set<std::string> romNoHle;  // nomes distintos
    for (const auto& ref : r.refs) {
        if (!ref.module || ref.device.rfind("rom", 0) != 0 || !ref.hle.empty()) continue;
        const std::string rom = rt::romModuleName(ref.device + ":" + ref.target);
        if (rom != "UDNL") romNoHle.insert(rom);
    }
    for (const auto& img : r.images) {
        for (const auto& m : img.modules) imgNoHle += m.irx && m.hle.empty() && !m.kernel ? 1u : 0u;
    }
    out << "\nResumo: " << irxNoHle << " de " << r.irx.size() << " IRX do disco sem HLE; " << romNoHle.size()
        << " módulo(s) da ROM referenciado(s) sem HLE; " << imgNoHle << " módulo(s) de imagens IOPRP sem HLE; "
        << r.elfs.size() << " outro(s) ELF(s) no disco.\n";
}

}  // namespace anyps2::disc
