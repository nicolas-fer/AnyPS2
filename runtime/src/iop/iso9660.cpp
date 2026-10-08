// Leitura de imagens ISO 9660 (ECMA-119): volume primário no setor 16,
// registros de diretório {len, extlen, lba LE@2, size LE@10, data@18,
// flags@25, namelen@32, nome@33}. DVD-9: segundo volume na camada 1.

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <utility>

#include "anyps2/common/error.h"
#include "anyps2/runtime/iop/iso9660.h"

namespace anyps2::rt {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

// Imagens de DVD passam de 4 GB: fseek com long não basta no Windows.
bool seek64(std::FILE* fp, std::uint64_t off) {
#ifdef _WIN32
    return _fseeki64(fp, static_cast<__int64>(off), SEEK_SET) == 0;
#else
    return fseeko(fp, static_cast<off_t>(off), SEEK_SET) == 0;
#endif
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// "ARQ.EXT;1" -> "ARQ.EXT"; "ARQ." -> "ARQ"
std::string baseName(const std::string& s) {
    std::string n = s.substr(0, s.find(';'));
    if (!n.empty() && n.back() == '.') n.pop_back();
    return upper(n);
}

IsoImage::Entry parseRecord(const std::uint8_t* r) {
    IsoImage::Entry e;
    e.lsn = le32(r + 2);
    e.size = le32(r + 10);
    std::memcpy(e.date.data(), r + 18, 7);
    e.flags = r[25];
    e.isDir = (r[25] & 2) != 0;
    e.name.assign(reinterpret_cast<const char*>(r + 33), r[32]);
    return e;
}

}  // namespace

IsoImage::~IsoImage() {
    if (fp_) std::fclose(fp_);
}

void IsoImage::open(const std::string& path) {
    if (fp_) std::fclose(fp_);
    fp_ = std::fopen(path.c_str(), "rb");
    if (!fp_) throw anyps2::Error("não foi possível abrir a imagem de disco '" + path + "' (ANYPS2_ISO)");
    path_ = path;
    std::error_code ec;
    const std::uint64_t bytes = std::filesystem::file_size(path, ec);
    // Descobre o formato pelo descritor de volume primário ("\1CD001").
    struct Layout {
        std::uint32_t raw, offset;
    };
    const Layout layouts[] = {{2048, 0}, {2352, 24}, {2352, 16}};
    bool found = false;
    for (const Layout& l : layouts) {
        std::uint8_t pvd[8] = {};
        if (!seek64(fp_, 16ull * l.raw + l.offset)) continue;
        if (std::fread(pvd, 1, sizeof(pvd), fp_) != sizeof(pvd)) continue;
        if (pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0) {
            rawSize_ = l.raw;
            dataOffset_ = l.offset;
            found = true;
            break;
        }
    }
    if (!found) {
        std::fclose(fp_);
        fp_ = nullptr;
        throw anyps2::Error("'" + path + "' não é uma imagem ISO 9660 (sem volume primário no setor 16)");
    }
    sectors_ = static_cast<std::uint32_t>(bytes / rawSize_);
    std::uint8_t pvd[kSectorSize];
    readSectors(16, 1, pvd);
    root_ = parseRecord(pvd + 156);
    root_.name = "\\";
    volumeId_.assign(reinterpret_cast<const char*>(pvd + 40), 32);
    while (!volumeId_.empty() && volumeId_.back() == ' ') volumeId_.pop_back();
    layer1Base_.reset();
    findLayer1(le32(pvd + 80));
}

// O volume da camada 0 declara só o tamanho dela (volume space size); se a
// imagem continua depois disso, procura o descritor primário da camada 1.
// Discos de PS2 gravam a base da camada 1 16 setores antes do fim do volume
// 0 (descritor exatamente no fim dele); a outra posição plausível é logo
// depois. A base só é aceita se o "." da raiz apontar para a própria raiz.
void IsoImage::findLayer1(std::uint32_t volumeSpace) {
    if (volumeSpace < 32 || volumeSpace >= sectors_) return;
    for (const std::uint32_t base : {volumeSpace - 16, volumeSpace}) {
        std::uint8_t pvd[kSectorSize];
        if (!readSectors(base + 16, 1, pvd)) continue;
        if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) continue;
        Entry r = parseRecord(pvd + 156);
        std::uint8_t dir[kSectorSize];
        if (!r.isDir || !readSectors(base + r.lsn, 1, dir) || dir[0] < 34 || le32(dir + 2) != r.lsn) continue;
        r.lsn += base;
        r.layer = 1;
        r.name = "\\";
        root1_ = r;
        layer1Base_ = base;
        return;
    }
}

bool IsoImage::readSectors(std::uint32_t lsn, std::uint32_t count, std::uint8_t* dst) {
    if (!fp_ || std::uint64_t{lsn} + count > sectors_) return false;
    if (rawSize_ == kSectorSize) {
        if (!seek64(fp_, std::uint64_t{lsn} * kSectorSize)) return false;
        return std::fread(dst, 1, std::size_t{count} * kSectorSize, fp_) == std::size_t{count} * kSectorSize;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!seek64(fp_, std::uint64_t{lsn + i} * rawSize_ + dataOffset_)) return false;
        if (std::fread(dst + std::size_t{i} * kSectorSize, 1, kSectorSize, fp_) != kSectorSize) return false;
    }
    return true;
}

bool IsoImage::readBytes(std::uint32_t lsn, std::uint64_t offset, std::uint32_t size, std::uint8_t* dst) {
    std::uint8_t sector[kSectorSize];
    while (size > 0) {
        const auto s = static_cast<std::uint32_t>(lsn + offset / kSectorSize);
        const auto in = static_cast<std::uint32_t>(offset % kSectorSize);
        const std::uint32_t n = std::min(size, kSectorSize - in);
        if (in == 0 && n == kSectorSize) {
            if (!readSectors(s, 1, dst)) return false;
        } else {
            if (!readSectors(s, 1, sector)) return false;
            std::memcpy(dst, sector + in, n);
        }
        dst += n;
        offset += n;
        size -= n;
    }
    return true;
}

std::vector<IsoImage::Entry> IsoImage::list(const Entry& dir) {
    std::vector<Entry> out;
    const std::uint32_t sectors = (dir.size + kSectorSize - 1) / kSectorSize;
    std::vector<std::uint8_t> data(std::size_t{sectors} * kSectorSize);
    if (!readSectors(dir.lsn, sectors, data.data())) return out;
    for (std::uint32_t s = 0; s < sectors; ++s) {
        std::size_t off = std::size_t{s} * kSectorSize;
        const std::size_t end = off + kSectorSize;
        // Registros não cruzam setores; len 0 = resto do setor vazio.
        while (off + 34 <= end && data[off] != 0) {
            const std::uint8_t len = data[off];
            if (len < 34 || off + len > end) break;
            const std::uint8_t namelen = data[off + 32];
            const bool dotEntry = namelen == 1 && (data[off + 33] == 0 || data[off + 33] == 1);
            if (!dotEntry) {
                Entry e = parseRecord(data.data() + off);
                if (dir.layer == 1) e.lsn += *layer1Base_;
                e.layer = dir.layer;
                out.push_back(std::move(e));
            }
            off += len;
        }
    }
    return out;
}

std::optional<IsoImage::Entry> IsoImage::lookup(const std::string& path, unsigned layer) {
    if (!fp_ || layer >= layerCount()) return std::nullopt;
    Entry cur = root(layer);
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == '\\' || path[pos] == '/')) ++pos;
        if (pos >= path.size()) break;
        const std::size_t next = path.find_first_of("\\/", pos);
        const std::string part = path.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
        pos = next == std::string::npos ? path.size() : next;
        if (!cur.isDir) return std::nullopt;
        const std::string want = baseName(part);
        bool found = false;
        for (const Entry& e : list(cur)) {
            if (baseName(e.name) == want) {
                cur = e;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return cur;
}

}  // namespace anyps2::rt
