// Rastreador do VU1 (ANYPS2_VU_TRACE e variáveis irmãs); ver vu_trace.h.
//
// Os valores dos registradores são os que o microprograma vê: VF e ACC em
// float do PS2 (denormais viram zero, Inf vira Fmax, como em ps2float.h) com
// os 32 bits de cada componente ao lado. O pacote GIF é decodificado com as
// mesmas regras de gif.cpp, mas por um decodificador próprio, para que o
// diagnóstico não dependa do código que está sendo investigado.
//
// Limite: as operações de Q/P ainda em voo (FDIV, EFU) e a fila de flags
// pendentes não aparecem; só os valores já confirmados nos registradores.

#include "anyps2/runtime/vu/vu_trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <sstream>
#include <string>

#include "anyps2/runtime/ps2float.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu/vu_core.h"

namespace anyps2::rt {

namespace {

void warn(const std::string& what) {
    std::fprintf(stderr, "anyps2: aviso: rastreador do VU1: %s\n", what.c_str());
}

constexpr std::uint64_t bits(std::uint64_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1ull << n) - 1);
}

// 0x e o valor com digits dígitos hexadecimais.
std::string hx(std::uint64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%0*llx", digits, static_cast<unsigned long long>(v));
    return buf;
}

std::string flt(float f) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.9g", static_cast<double>(f));
    return buf;
}

// Float do VU (formato do PS2) e float IEEE (GS: ST e Q do GIF).
float vuFloat(std::uint32_t b) { return ps2f::in(b); }
float ieeeFloat(std::uint32_t b) {
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

bool parseU64(const char* s, std::uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 0);
    if (end == s || *end != '\0') return false;
    out = v;
    return true;
}

// Lista de endereços de micro memória (até 0xFFFF), separados por vírgula ou espaço.
bool parseKicks(const std::string& text, std::vector<std::uint32_t>& out) {
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t end = text.find_first_of(", ", pos);
        if (end == std::string::npos) end = text.size();
        if (end > pos) {
            std::uint64_t v = 0;
            if (!parseU64(text.substr(pos, end - pos).c_str(), v) || v > 0xFFFF) return false;
            out.push_back(static_cast<std::uint32_t>(v));
        }
        pos = end + 1;
    }
    return !out.empty();
}

// Registradores de VF/VI/ACC e os de controle (vi[] com os índices de reg::).
void writeRegs(std::ostream& o, const Reg128* vf, const std::uint32_t* vi, const Reg128* acc) {
    char label[16];
    for (unsigned i = 0; i < 32; ++i) {
        const Reg128& r = vf[i];
        std::snprintf(label, sizeof label, "%02u", i);
        o << "  VF" << label << ": x=" << flt(vuFloat(r.uw[0])) << " y=" << flt(vuFloat(r.uw[1]))
          << " z=" << flt(vuFloat(r.uw[2])) << " w=" << flt(vuFloat(r.uw[3])) << "  [" << hx(r.uw[0], 8) << " "
          << hx(r.uw[1], 8) << " " << hx(r.uw[2], 8) << " " << hx(r.uw[3], 8) << "]\n";
    }
    o << "  ACC: x=" << flt(vuFloat(acc->uw[0])) << " y=" << flt(vuFloat(acc->uw[1]))
      << " z=" << flt(vuFloat(acc->uw[2])) << " w=" << flt(vuFloat(acc->uw[3])) << "  [" << hx(acc->uw[0], 8)
      << " " << hx(acc->uw[1], 8) << " " << hx(acc->uw[2], 8) << " " << hx(acc->uw[3], 8) << "]\n";
    for (unsigned i = 0; i < 16; i += 4) {
        o << "  ";
        for (unsigned j = i; j < i + 4; ++j) {
            std::snprintf(label, sizeof label, "%02u", j);
            o << "vi" << label << "=" << hx(vi[j], 8) << (j + 1 < i + 4 ? " " : "");
        }
        o << "\n";
    }
    using namespace vucore::reg;
    o << "  flags: Status=" << hx(vi[Status], 8) << " Mac=" << hx(vi[Mac], 8) << " Clip=" << hx(vi[Clip], 8)
      << "\n";
    o << "  R=" << hx(vi[R], 8) << " (" << flt(vuFloat(vi[R])) << ")  I=" << hx(vi[I], 8) << " ("
      << flt(vuFloat(vi[I])) << ")  Q=" << hx(vi[Q], 8) << " (" << flt(vuFloat(vi[Q])) << ")  P=" << hx(vi[P], 8)
      << " (" << flt(vuFloat(vi[P])) << ")  TPC=" << hx(vi[TPC], 8) << "\n";
}

// Qwords diferentes de zero; o endereço é em qwords (e o byte entre parênteses).
void writeMem(std::ostream& o, const std::uint8_t* d, std::uint32_t size) {
    unsigned n = 0;
    for (std::uint32_t at = 0; at + 16 <= size; at += 16) {
        std::uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, d + at, 8);
        std::memcpy(&hi, d + at + 8, 8);
        if (lo == 0 && hi == 0) continue;
        o << "  qw " << hx(at / 16, 4) << " (byte " << hx(at, 4) << "): " << hx(lo, 16) << " " << hx(hi, 16)
          << "\n";
        ++n;
    }
    if (n == 0) o << "  (todos zero)\n";
}

const char* flgName(unsigned flg) {
    switch (flg) {
        case 0: return "PACKED";
        case 1: return "REGLIST";
        default: return "IMAGE";
    }
}

const char* packedName(unsigned desc) {
    static const char* const names[16] = {"PRIM",   "RGBAQ",  "ST",    "UV",     "XYZF2", "XYZ2",
                                          "TEX0_1", "TEX0_2", "CLAMP_1", "CLAMP_2", "FOG",  "reservado",
                                          "XYZF3",  "XYZ3",   "A+D",   "NOP"};
    return names[desc & 0xF];
}

std::string px(std::uint64_t v) {
    return std::to_string(v) + " (" + flt(static_cast<float>(v) / 16.0f) + " px)";
}

// Um registrador do modo PACKED: os campos do descritor e as palavras de 32 bits.
// q é o Q do último ST (IEEE), que o RGBAQ carrega.
std::string describePacked(unsigned desc, std::uint64_t lo, std::uint64_t hi, std::uint32_t& q) {
    std::ostringstream s;
    switch (desc) {
        case 0x0:
            s << "PRIM=" << bits(lo, 0, 11) << " " << hx(bits(lo, 0, 11), 3);
            break;
        case 0x1:
            s << "R=" << bits(lo, 0, 8) << " G=" << bits(lo, 32, 8) << " B=" << bits(hi, 0, 8)
              << " A=" << bits(hi, 32, 8) << " Q=" << flt(ieeeFloat(q)) << " (do último ST)";
            break;
        case 0x2:
            q = static_cast<std::uint32_t>(hi);
            s << "S=" << flt(ieeeFloat(static_cast<std::uint32_t>(lo))) << " T="
              << flt(ieeeFloat(static_cast<std::uint32_t>(lo >> 32))) << " Q=" << flt(ieeeFloat(q));
            break;
        case 0x3:
            s << "U=" << bits(lo, 0, 14) << " V=" << bits(lo, 32, 14);
            break;
        case 0x4: case 0xC:
            s << "X=" << px(bits(lo, 0, 16)) << " Y=" << px(bits(lo, 32, 16)) << " Z=" << bits(hi, 4, 24)
              << " F=" << bits(hi, 36, 8) << " ADC=" << bits(hi, 47, 1);
            break;
        case 0x5: case 0xD:
            s << "X=" << px(bits(lo, 0, 16)) << " Y=" << px(bits(lo, 32, 16)) << " Z=" << bits(hi, 0, 32)
              << " ADC=" << bits(hi, 47, 1);
            break;
        case 0x6: case 0x7: case 0x8: case 0x9:
            s << "valor " << hx(lo, 16);
            break;
        case 0xA:
            s << "F=" << bits(hi, 36, 8);
            break;
        case 0xE:
            s << "registrador " << hx(bits(hi, 0, 8), 2) << " = " << hx(lo, 16);
            break;
        case 0xB:
            s << "descritor reservado";
            break;
        default:
            s << "sem efeito";
            break;
    }
    const std::uint32_t w0 = static_cast<std::uint32_t>(lo), w1 = static_cast<std::uint32_t>(lo >> 32);
    const std::uint32_t w2 = static_cast<std::uint32_t>(hi), w3 = static_cast<std::uint32_t>(hi >> 32);
    s << "  [" << hx(w0, 8) << " " << hx(w1, 8) << " " << hx(w2, 8) << " " << hx(w3, 8) << "]";
    return s.str();
}

// Decodifica o pacote que um XGKICK enviaria a partir de addr (mesmo laço de
// Gif::kick: um qword por vez, com o endereço dando a volta na memória).
void writeGifPacket(std::ostream& o, const std::uint8_t* mem, std::uint32_t size, std::uint32_t addr) {
    bool active = false, eop = false;
    unsigned nloop = 0, nreg = 0, reg = 0, flg = 0;
    std::uint64_t regs = 0;
    std::uint32_t q = 0x3F800000u;  // Q volta a 1.0 em cada GIFtag
    for (std::uint32_t n = 0; n <= size / 16; ++n) {
        const std::uint32_t at = (addr + 16u * n) & (size - 16);
        std::uint64_t lo = 0, hi = 0;
        std::memcpy(&lo, mem + at, 8);
        std::memcpy(&hi, mem + at + 8, 8);
        if (!active) {
            nloop = static_cast<unsigned>(bits(lo, 0, 15));
            eop = bits(lo, 15, 1) != 0;
            flg = static_cast<unsigned>(bits(lo, 58, 2));
            nreg = static_cast<unsigned>(bits(lo, 60, 4));
            if (nreg == 0) nreg = 16;
            regs = hi;
            reg = 0;
            q = 0x3F800000u;
            active = nloop > 0;
            o << "  GIFtag (qword " << hx(at / 16, 4) << "): NLOOP=" << nloop << " EOP=" << (eop ? 1 : 0)
              << " PRE=" << bits(lo, 46, 1) << " PRIM=" << hx(bits(lo, 47, 11), 3) << " FLG=" << flg << " ("
              << flgName(flg) << ") NREG=" << nreg << " REGS=" << hx(regs, 16) << "\n";
        } else if (flg == 0) {
            const unsigned desc = static_cast<unsigned>((regs >> (reg * 4)) & 0xF);
            o << "  dado " << reg << " " << packedName(desc) << ": " << describePacked(desc, lo, hi, q) << "\n";
            if (++reg == nreg) {
                reg = 0;
                if (--nloop == 0) active = false;
            }
        } else if (flg == 1) {
            const std::uint64_t words[2] = {lo, hi};
            for (std::uint64_t w : words) {
                if (!active) break;
                const unsigned desc = static_cast<unsigned>((regs >> (reg * 4)) & 0xF);
                o << "  reglist registrador " << hx(desc, 1) << ": " << hx(w, 16) << "\n";
                if (++reg == nreg) {
                    reg = 0;
                    if (--nloop == 0) active = false;
                }
            }
        } else {
            o << "  imagem: " << hx(lo, 16) << " " << hx(hi, 16) << "\n";
            if (--nloop == 0) active = false;
        }
        if (!active && eop) return;
    }
    o << "  (sem EOP dentro da memória: o XGKICK real lança erro)\n";
}

}  // namespace

std::unique_ptr<VuTrace> VuTrace::fromEnv() {
    const char* file = std::getenv("ANYPS2_VU_TRACE");
    if (!file || !*file) return nullptr;
    VuTraceConfig cfg;
    cfg.out = file;
    if (const char* v = std::getenv("ANYPS2_VU_TRACE_FROM")) {
        if (!parseU64(v, cfg.from)) warn("ANYPS2_VU_TRACE_FROM inválido; usando 0");
    }
    if (const char* v = std::getenv("ANYPS2_VU_TRACE_TO")) {
        if (!parseU64(v, cfg.to)) warn("ANYPS2_VU_TRACE_TO inválido; sem limite superior");
    }
    if (const char* v = std::getenv("ANYPS2_VU_TRACE_KICK")) {
        if (*v && !parseKicks(v, cfg.kicks)) {
            warn("ANYPS2_VU_TRACE_KICK espera endereços de micro memória (até 0xFFFF) separados por vírgula; rastreador desligado");
            return nullptr;
        }
    }
    if (const char* v = std::getenv("ANYPS2_VU_TRACE_MAX")) {
        std::uint64_t m = 0;
        if (parseU64(v, m) && m > 0) {
            cfg.maxPrograms = static_cast<unsigned>(std::min<std::uint64_t>(m, 1u << 20));
        } else {
            warn("ANYPS2_VU_TRACE_MAX inválido; usando " + std::to_string(kDefaultMaxPrograms));
        }
    }
    auto trace = std::make_unique<VuTrace>();
    trace->configure(cfg);
    if (!trace->active()) return nullptr;
    return trace;
}

void VuTrace::configure(const VuTraceConfig& cfg) {
    out_.close();
    cfg_ = cfg;
    runs_ = 0;
    recorded_ = 0;
    run_ = Snapshot{};
    out_.open(cfg.out, std::ios::out | std::ios::trunc);
    if (!out_.is_open()) warn("não consegui abrir " + cfg.out + "; rastreador desligado");
}

void VuTrace::beginRun(Vu& vu, std::uint32_t startPc, std::uint32_t eePc, std::uint64_t vblank) {
    run_.active = false;
    if (!active() || vblank < cfg_.from || vblank > cfg_.to) return;
    ++runs_;
    if (recorded_ >= cfg_.maxPrograms) return;  // o limite de microprogramas gravados já foi atingido
    run_.active = true;
    run_.recorded = false;
    run_.number = runs_;
    run_.vblank = vblank;
    run_.cycle = vu.cycle();
    run_.startPc = startPc;
    run_.eePc = eePc;
    const vucore::Regs& r = vu.regs();
    for (unsigned i = 0; i < 32; ++i) run_.vf[i] = r.vf[i];
    for (unsigned i = 0; i < 32; ++i) run_.vi[i] = r.vi[i];
    run_.acc = *r.acc;
    run_.data.assign(vu.data(), vu.data() + vu.dataSize());
}

void VuTrace::kick(Vu& vu, std::uint32_t addr, std::uint32_t pc) {
    if (!run_.active) return;
    if (!cfg_.kicks.empty() && std::find(cfg_.kicks.begin(), cfg_.kicks.end(), pc) == cfg_.kicks.end()) return;
    if (!run_.recorded) {  // a primeira gravação deste microprograma conta para o limite
        run_.recorded = true;
        ++recorded_;
    }

    std::ostringstream o;
    o << "=== VU1 microprograma #" << run_.number << " | VBlank " << run_.vblank << " | início " << hx(run_.startPc, 4)
      << " | EE em " << hx(run_.eePc, 8) << " | XGKICK em " << hx(pc, 4) << " | ciclos " << run_.cycle << " -> "
      << vu.cycle() << "\n";
    o << "-- entrada (início do microprograma):\n";
    writeRegs(o, run_.vf, run_.vi, &run_.acc);
    o << "-- memória de dados na entrada:\n";
    writeMem(o, run_.data.data(), static_cast<std::uint32_t>(run_.data.size()));
    o << "-- no XGKICK (endereço de dados " << hx(addr, 4) << "):\n";
    const vucore::Regs& r = vu.regs();
    writeRegs(o, r.vf, r.vi, r.acc);
    o << "-- memória de dados no XGKICK:\n";
    writeMem(o, vu.data(), vu.dataSize());
    o << "-- pacote GIF enviado pelo XGKICK:\n";
    writeGifPacket(o, vu.data(), vu.dataSize(), addr);
    o << "\n";
    out_ << o.str();
    out_.flush();
}

}  // namespace anyps2::rt
