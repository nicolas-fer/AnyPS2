#include "anyps2/runtime/iop/mcserv.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;
namespace fs = std::filesystem;

namespace {

// Operações (libmc.c: numeração do XMCSERV e do MCSERV antigo).
enum Op { INIT, GET_INFO, OPEN, CLOSE, SEEK, READ, WRITE, FLUSH, CH_DIR, GET_DIR, SET_INFO, DELETE, FORMAT,
          UNFORMAT, GET_ENT, GET_SLOT_MAX, OTHER };
Op decode(std::uint32_t fn, bool& oldProtocol) {
    oldProtocol = fn >= 0x70 && fn <= 0x80;
    switch (fn) {
        case 0xFE: case 0x70: return INIT;
        case 0x01: case 0x78: return GET_INFO;
        case 0x02: case 0x71: return OPEN;
        case 0x03: case 0x72: return CLOSE;
        case 0x04: case 0x75: return SEEK;
        case 0x05: case 0x73: return READ;
        case 0x06: case 0x74: return WRITE;
        case 0x0A: case 0x7A: return FLUSH;
        case 0x0C: case 0x7B: return CH_DIR;
        case 0x0D: case 0x76: return GET_DIR;
        case 0x0E: case 0x7C: return SET_INFO;
        case 0x0F: case 0x79: return DELETE;
        case 0x10: case 0x77: return FORMAT;
        case 0x11: case 0x80: return UNFORMAT;
        case 0x12: return GET_ENT;
        case 0x15: return GET_SLOT_MAX;  // libmc do SDK 3.0 (sceMcGetSlotMax)
        default: return OTHER;
    }
}

// Códigos de resultado do mcman.
constexpr std::int32_t kOk = 0, kChangedCard = -1, kNoFormat = -2, kNoEntry = -4, kDeniedPermit = -5,
                       kNotEmpty = -6, kNoCard = -10;
// Atributos (libmc-common.h).
constexpr std::uint16_t kAttrRwx = 0x0007, kAttrDupProhibit = 0x0008, kAttrFile = 0x0010, kAttrSubdir = 0x0020,
                        kAttrClosed = 0x0080, kAttrExists = 0x8000;
constexpr std::uint16_t kAttrFileEntry = kAttrExists | 0x0400 | kAttrClosed | kAttrFile | kAttrRwx;
constexpr std::uint16_t kAttrDirEntry = kAttrExists | 0x0400 | kAttrSubdir | kAttrRwx;
// Cartão de 8 MB: clusters de 1 KB livres num cartão recém-formatado.
constexpr std::int32_t kTotalClusters = 8135;
// Flags de abertura (sceMcFile*).
constexpr std::uint32_t kOpenWrite = 0x02, kOpenCreate = 0x200, kOpenTrunc = 0x400, kCreateDir = 0x40;
constexpr std::uint32_t kInfoName = 0x10;  // sceMcFileInfo* para renomear (mcman: flags & 0x10)

bool globMatch(const char* pat, const char* s) {
    if (!*pat) return !*s;
    if (*pat == '*') return globMatch(pat + 1, s) || (*s && globMatch(pat, s + 1));
    if (*s && (*pat == '?' || *pat == *s)) return globMatch(pat + 1, s + 1);
    return false;
}

void putDate(std::vector<std::uint8_t>& e, std::size_t off, fs::file_time_type t) {
    // Horário do cartão: JST (UTC+9), como o relógio do console.
    const auto sys = std::chrono::system_clock::now() +
                     std::chrono::duration_cast<std::chrono::system_clock::duration>(t - fs::file_time_type::clock::now());
    std::time_t tt = std::chrono::system_clock::to_time_t(sys) + 9 * 3600;
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &tt);
#else
    gmtime_r(&tt, &tm);
#endif
    e[off + 1] = static_cast<std::uint8_t>(tm.tm_sec);
    e[off + 2] = static_cast<std::uint8_t>(tm.tm_min);
    e[off + 3] = static_cast<std::uint8_t>(tm.tm_hour);
    e[off + 4] = static_cast<std::uint8_t>(tm.tm_mday);
    e[off + 5] = static_cast<std::uint8_t>(tm.tm_mon + 1);
    const auto year = static_cast<std::uint16_t>(tm.tm_year + 1900);
    e[off + 6] = static_cast<std::uint8_t>(year);
    e[off + 7] = static_cast<std::uint8_t>(year >> 8);
}

}  // namespace

McServ::McServ(Iop& iop) : iop_(iop) {}

McServ::~McServ() {
    for (auto& [fd, f] : files_) {
        if (f.fp) std::fclose(f.fp);
    }
}

void McServ::reset() {
    for (auto& [fd, f] : files_) {
        if (f.fp) std::fclose(f.fp);
    }
    files_.clear();
    curDir_[0] = curDir_[1] = "/";
    infoSeen_[0] = infoSeen_[1] = false;
}

fs::path McServ::root(unsigned port) const {
    fs::path base;
    if (const char* d = std::getenv("ANYPS2_MC_DIR")) {
        base = d;
    } else {
        const char* host = std::getenv("ANYPS2_HOST_DIR");
        base = fs::path(host ? host : ".") / "memcard";
    }
    return base / ("mc" + std::to_string(port));
}

void McServ::registerServer() {
    // O cartão da porta 1 existe se a pasta mc0 existir; ela é criada na
    // primeira vez (como um cartão novo já formatado). A porta 2 (mc1) só
    // tem cartão se a pasta existir.
    std::error_code ec;
    fs::create_directories(root(0), ec);
    iop_.registerServer(0x80000400u, "mcserv", [this](std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                      std::uint32_t pc) { return rpc(fn, in, pc); });
}

std::string McServ::resolve(unsigned port, const std::string& name) const {
    std::string full = (!name.empty() && name[0] == '/') ? name : curDir_[port] + "/" + name;
    std::vector<std::string> parts;
    std::size_t i = 0;
    while (i <= full.size()) {
        const std::size_t j = full.find('/', i);
        const std::string seg = full.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string out;
    for (const auto& p : parts) out += "/" + p;
    return out.empty() ? "/" : out;
}

fs::path McServ::hostPath(unsigned port, const std::string& cardPath) const {
    fs::path p = root(port);
    if (cardPath != "/") p /= cardPath.substr(1);
    return p;
}

std::int32_t McServ::freeClusters(unsigned port) const {
    std::int64_t used = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(root(port), ec); it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        if (ec) break;
        if (it->is_directory(ec)) used += 1;
        else used += static_cast<std::int64_t>((it->file_size(ec) + 1023) / 1024);
    }
    return static_cast<std::int32_t>(std::max<std::int64_t>(0, kTotalClusters - used));
}

std::vector<std::uint8_t> McServ::tableEntry(const fs::path& p, const std::string& name) const {
    std::vector<std::uint8_t> e(64, 0);
    std::error_code ec;
    const bool dir = fs::is_directory(p, ec);
    const auto t = fs::last_write_time(p, ec);
    if (!ec) {
        putDate(e, 0, t);
        putDate(e, 8, t);
    }
    wr32(e, 16, dir ? 0u : static_cast<std::uint32_t>(fs::file_size(p, ec)));
    const std::uint16_t attr = dir ? kAttrDirEntry : kAttrFileEntry;
    e[20] = static_cast<std::uint8_t>(attr);
    e[21] = static_cast<std::uint8_t>(attr >> 8);
    std::memcpy(e.data() + 32, name.c_str(), std::min<std::size_t>(name.size(), 31));
    return e;
}

std::optional<std::vector<std::uint8_t>> McServ::rpc(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                     std::uint32_t pc) {
    bool oldProtocol = false;
    const Op op = decode(fn, oldProtocol);
    Memory& m = iop_.runtime().memory();
    std::error_code ec;
    if (iop_.runtime().options().traceIop) std::fprintf(stderr, "[iop] mcserv 0x%02x\n", fn);

    // Layouts: mcDescParam_t {fd, port, slot, size, offset, origin, buffer, param, data[16]}
    // e libmc_name_param_stru {port, slot, flags, maxent, mcT/curdir, name[1024]}.
    const auto descFd = static_cast<std::int32_t>(rd32(in, 0));
    const unsigned descPort = rd32(in, 4) & 1;
    const unsigned namePort = rd32(in, 0) & 1;
    const std::string name = cstr(in, 20, 1024);
    auto cardPresent = [&](unsigned port) { return fs::is_directory(root(port), ec); };

    switch (op) {
        case GET_SLOT_MAX:  // [4] porta -> slots na porta: 1 (sem multitap)
            return result32(rd32(in, 4) < 2 ? 1 : -1);
        case INIT: {
            std::vector<std::uint8_t> out(12, 0);
            wr32(out, 0, 0);
            // Versões declaradas: a libmc da Sony do SDK 3.0 exige mcserv >= 0x20A e
            // mcman >= 0x20E ("too old release"); a do ps2sdk só checa mínimos menores.
            wr32(out, 4, 0x020A);  // versão do mcserv
            wr32(out, 8, 0x020E);  // versão do mcman
            return out;
        }
        case GET_INFO: {
            const std::uint32_t param = rd32(in, 28);
            const bool present = cardPresent(descPort);
            std::vector<std::uint8_t> end(oldProtocol ? 64 : 192, 0);
            wr32(end, 0, present ? 2 : 0);  // sceMcTypePS2
            wr32(end, 4, static_cast<std::uint32_t>(present ? freeClusters(descPort) : 0));
            if (!oldProtocol) wr32(end, 144, present ? 1 : 0);
            m.copyToGuest(param, end.data(), static_cast<std::uint32_t>(end.size()), pc);
            std::int32_t r = kNoCard;
            if (present) {
                r = infoSeen_[descPort] ? kOk : kChangedCard;
                infoSeen_[descPort] = true;
            }
            return result32(r);
        }
        case OPEN: {
            if (!cardPresent(namePort)) return result32(kNoCard);
            const std::uint32_t flags = rd32(in, 8);
            const std::string path = resolve(namePort, name);
            const fs::path host = hostPath(namePort, path);
            if (flags & kCreateDir) {  // mcMkDir
                if (fs::exists(host, ec) || !fs::is_directory(host.parent_path(), ec)) return result32(kNoEntry);
                fs::create_directory(host, ec);
                return result32(ec ? kNoEntry : kOk);
            }
            if (fs::is_directory(host, ec)) return result32(kNoEntry);
            const bool exists = fs::exists(host, ec);
            if (!exists && !(flags & kOpenCreate)) return result32(kNoEntry);
            if (!fs::is_directory(host.parent_path(), ec)) return result32(kNoEntry);
            const char* mode = "rb";
            if (flags & kOpenWrite) mode = (!exists || (flags & kOpenTrunc)) ? "w+b" : "r+b";
            std::FILE* fp = std::fopen(host.string().c_str(), mode);
            if (!fp) return result32(kDeniedPermit);
            const std::int32_t fd = nextFd_++;
            files_[fd] = {fp, namePort, path};
            return result32(fd);
        }
        case CLOSE: {
            auto it = files_.find(descFd);
            if (it == files_.end()) return result32(kDeniedPermit);
            std::fclose(it->second.fp);
            files_.erase(it);
            return result32(kOk);
        }
        case FLUSH: {
            auto it = files_.find(descFd);
            if (it == files_.end()) return result32(kDeniedPermit);
            std::fflush(it->second.fp);
            return result32(kOk);
        }
        case SEEK: {
            auto it = files_.find(descFd);
            if (it == files_.end()) return result32(kDeniedPermit);
            const auto offset = static_cast<std::int32_t>(rd32(in, 16));
            const std::uint32_t origin = rd32(in, 20);
            if (origin > 2 || std::fseek(it->second.fp, offset, static_cast<int>(origin)) != 0) {
                return result32(kDeniedPermit);
            }
            return result32(static_cast<std::int32_t>(std::ftell(it->second.fp)));
        }
        case READ: {
            auto it = files_.find(descFd);
            if (it == files_.end()) return result32(kDeniedPermit);
            const std::uint32_t size = rd32(in, 12), buffer = rd32(in, 24), param = rd32(in, 28);
            std::vector<std::uint8_t> buf(size);
            const std::size_t got = std::fread(buf.data(), 1, size, it->second.fp);
            if (got) m.copyToGuest(buffer, buf.data(), static_cast<std::uint32_t>(got), pc);
            // Pontas desalinhadas já foram escritas: size1 = size2 = 0.
            const std::uint32_t zero[4] = {0, 0, 0, 0};
            if (param) m.copyToGuest(param, zero, sizeof(zero), pc);
            return result32(static_cast<std::int32_t>(got));
        }
        case WRITE: {  // prefixo desalinhado em data[] (origin bytes) + resto alinhado no EE
            auto it = files_.find(descFd);
            if (it == files_.end()) return result32(kDeniedPermit);
            const std::uint32_t size = rd32(in, 12), head = std::min<std::uint32_t>(rd32(in, 20), 16);
            const std::uint32_t buffer = rd32(in, 24);
            std::vector<std::uint8_t> buf(head + size);
            if (head) std::memcpy(buf.data(), in.data() + 32, head);
            if (size) m.copyFromGuest(buf.data() + head, buffer, size, pc);
            const std::size_t written = std::fwrite(buf.data(), 1, buf.size(), it->second.fp);
            return result32(static_cast<std::int32_t>(written));
        }
        case CH_DIR: {  // devolve o novo diretório atual em m_curdir
            if (!cardPresent(namePort)) return result32(kNoCard);
            const std::string path = name.empty() ? curDir_[namePort] : resolve(namePort, name);
            if (!fs::is_directory(hostPath(namePort, path), ec)) return result32(kNoEntry);
            curDir_[namePort] = path;
            const std::uint32_t dst = rd32(in, 16);
            if (dst) m.copyToGuest(dst, path.c_str(), static_cast<std::uint32_t>(path.size() + 1), pc);
            return result32(kOk);
        }
        case GET_DIR: {
            if (!cardPresent(namePort)) return result32(kNoCard);
            const std::uint32_t flags = rd32(in, 8) & 0xFFFF, table = rd32(in, 16);
            const auto maxent = static_cast<std::int32_t>(rd32(in, 12));
            DirListing& l = listing_[namePort];
            if (flags == 0) {
                l = {};
                const std::string full = resolve(namePort, name);
                const auto slash = full.rfind('/');
                const std::string dir = full.substr(0, slash == 0 ? 1 : slash);
                const std::string pattern = full.substr(slash + 1);
                const fs::path hostDir = hostPath(namePort, dir);
                if (!fs::is_directory(hostDir, ec)) return result32(kNoEntry);
                if (dir != "/") {
                    if (globMatch(pattern.c_str(), ".")) l.entries.push_back(tableEntry(hostDir, "."));
                    if (globMatch(pattern.c_str(), "..")) l.entries.push_back(tableEntry(hostDir.parent_path(), ".."));
                }
                std::vector<std::string> names;
                for (const auto& e : fs::directory_iterator(hostDir, ec)) names.push_back(e.path().filename().string());
                std::sort(names.begin(), names.end());
                for (const auto& n : names) {
                    if (globMatch(pattern.c_str(), n.c_str())) l.entries.push_back(tableEntry(hostDir / n, n));
                }
            }
            std::int32_t count = 0;
            while (count < maxent && l.next < l.entries.size()) {
                m.copyToGuest(table + static_cast<std::uint32_t>(count) * 64, l.entries[l.next].data(), 64, pc);
                ++l.next;
                ++count;
            }
            return result32(count);
        }
        case SET_INFO: {  // {.., flags, .., mcT, name}: só renomear tem efeito no host
            if (!cardPresent(namePort)) return result32(kNoCard);
            const std::uint32_t flags = rd32(in, 8);
            const std::string path = resolve(namePort, name);
            const fs::path host = hostPath(namePort, path);
            if (!fs::exists(host, ec)) return result32(kNoEntry);
            if (flags & kInfoName) {
                std::uint8_t entry[64];
                m.copyFromGuest(entry, rd32(in, 16), 64, pc);
                const std::string newName(reinterpret_cast<const char*>(entry + 32),
                                          strnlen(reinterpret_cast<const char*>(entry + 32), 32));
                const fs::path target = host.parent_path() / newName;
                if (fs::exists(target, ec)) return result32(kNoEntry);
                fs::rename(host, target, ec);
                if (ec) return result32(kDeniedPermit);
            }
            // Datas e atributos (proteção contra cópia etc.) não têm onde ser
            // guardados no sistema de arquivos do host: são aceitos e ignorados.
            return result32(kOk);
        }
        case DELETE: {
            if (!cardPresent(namePort)) return result32(kNoCard);
            const fs::path host = hostPath(namePort, resolve(namePort, name));
            if (!fs::exists(host, ec)) return result32(kNoEntry);
            if (fs::is_directory(host, ec) && !fs::is_empty(host, ec)) return result32(kNotEmpty);
            fs::remove(host, ec);
            return result32(ec ? kDeniedPermit : kOk);
        }
        case FORMAT:
            fs::remove_all(root(descPort), ec);
            fs::create_directories(root(descPort), ec);
            curDir_[descPort] = "/";
            return result32(ec ? kDeniedPermit : kOk);
        case UNFORMAT:
            fs::remove_all(root(descPort), ec);
            return result32(kOk);
        default:
            throw Unimplemented("mcserv: função " + anyps2::hex(fn) +
                                    " (libmc) não implementada no HLE do memory card",
                                pc);
    }
    (void)kNoFormat;
    (void)kAttrDupProhibit;
}

}  // namespace anyps2::rt
