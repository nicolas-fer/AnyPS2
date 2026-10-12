// Gravação e replay das execuções do VU1; ver vu_record.h (inclui o formato do arquivo).

#include "anyps2/runtime/vu/vu_record.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>

#include "anyps2/common/error.h"
#include "anyps2/runtime/vu/vu_core.h"

namespace anyps2::rt {

namespace {

constexpr char kMagic[4] = {'A', 'V', 'U', 'R'};
constexpr std::uint8_t kRecMicro = 1, kRecEntry = 2;

// Hash de 64 bits por palavra (não criptográfico; serve para detectar diferenças).
struct Hasher {
    std::uint64_t h = 0xCBF29CE484222325ull;
    void word(std::uint64_t w) { h = (std::rotl(h, 29) ^ w) * 0x9E3779B97F4A7C15ull; }
    void bytes(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        word(n);
        for (; n >= 8; n -= 8, b += 8) {
            std::uint64_t w;
            std::memcpy(&w, b, 8);
            word(w);
        }
        std::uint64_t t = 0;
        if (n) std::memcpy(&t, b, n);
        word(t);
    }
    std::uint64_t done() const { return h ^ (h >> 32); }
};

std::uint64_t hashBytes(const void* p, std::size_t n) {
    Hasher h;
    h.bytes(p, n);
    return h.done();
}

// ---- escrita ----------------------------------------------------------------

struct Writer {
    std::vector<std::uint8_t> buf;
    void raw(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
    template <class T>
    void put(T v) {  // o formato é little-endian; as plataformas suportadas também são
        raw(&v, sizeof v);
    }
};

// ---- leitura ----------------------------------------------------------------

struct Reader {
    const std::vector<std::uint8_t>& buf;
    std::size_t pos = 0;
    const std::string& path;
    void need(std::size_t n) const {
        if (buf.size() - pos < n) throw anyps2::Error("gravação do VU truncada: " + path);
    }
    void raw(void* p, std::size_t n) {
        need(n);
        std::memcpy(p, buf.data() + pos, n);
        pos += n;
    }
    template <class T>
    T get() {
        T v;
        raw(&v, sizeof v);
        return v;
    }
    bool atEnd() const { return pos >= buf.size(); }
};

bool parseRange(const char* s, std::uint64_t& from, std::uint64_t& to) {
    from = 0;
    to = ~std::uint64_t{0};
    const char* dash = std::strchr(s, '-');
    char* end = nullptr;
    if (!dash) {
        const unsigned long long v = std::strtoull(s, &end, 0);
        if (end == s || *end != '\0') return false;
        from = to = v;
        return true;
    }
    if (dash != s) {
        from = std::strtoull(s, &end, 0);
        if (end != dash) return false;
    }
    if (dash[1] != '\0') {
        to = std::strtoull(dash + 1, &end, 0);
        if (*end != '\0') return false;
    }
    return from <= to;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gravador
// ---------------------------------------------------------------------------

std::unique_ptr<VuRecorder> VuRecorder::fromEnv() {
    const char* file = std::getenv("ANYPS2_VU_RECORD");
    if (!file || !*file) return nullptr;
    std::uint64_t from = 0, to = ~std::uint64_t{0};
    if (const char* v = std::getenv("ANYPS2_VU_RECORD_FRAMES"); v && *v) {
        if (!parseRange(v, from, to)) {
            std::fprintf(stderr, "anyps2: aviso: ANYPS2_VU_RECORD_FRAMES espera a, a-b, a- ou -b; gravador desligado\n");
            return nullptr;
        }
    }
    auto rec = std::make_unique<VuRecorder>();
    if (!rec->open(file, from, to)) {
        std::fprintf(stderr, "anyps2: aviso: não consegui abrir %s; gravador do VU desligado\n", file);
        return nullptr;
    }
    return rec;
}

bool VuRecorder::open(const std::string& path, std::uint64_t from, std::uint64_t to) {
    out_.close();
    out_.clear();
    from_ = from;
    to_ = to;
    entries_ = 0;
    headerDone_ = false;
    micros_.clear();
    out_.open(path, std::ios::binary | std::ios::trunc);
    return out_.is_open();
}

void VuRecorder::record(Vu& vu, std::uint32_t startPc, std::uint32_t eePc, std::uint64_t vblank) {
    if (!out_.is_open() || vblank < from_ || vblank > to_) return;
    Writer w;
    if (!headerDone_) {
        headerDone_ = true;
        w.raw(kMagic, 4);
        w.put<std::uint32_t>(kVuRecVersion);
        w.put<std::uint32_t>(vu.microSize());
        w.put<std::uint32_t>(vu.dataSize());
        w.put<std::uint32_t>(vu.unit());
    }
    const std::uint64_t mh = hashBytes(vu.micro(), vu.microSize());
    if (std::find(micros_.begin(), micros_.end(), mh) == micros_.end()) {
        micros_.push_back(mh);
        w.put<std::uint8_t>(kRecMicro);
        w.put<std::uint64_t>(mh);
        w.raw(vu.micro(), vu.microSize());
    }
    w.put<std::uint8_t>(kRecEntry);
    w.put<std::uint64_t>(vblank);
    w.put<std::uint32_t>(startPc);
    w.put<std::uint32_t>(eePc);
    w.put<std::uint64_t>(mh);
    w.put<std::uint32_t>(vu.vifTopValue());
    w.put<std::uint32_t>(vu.vifItopValue());
    w.put<std::uint64_t>(vu.cycle());
    const vucore::Regs& r = vu.regs();
    w.raw(r.vf, 32 * sizeof(Reg128));
    w.raw(r.vi, 32 * sizeof(std::uint32_t));
    w.raw(r.acc, sizeof(Reg128));
    const VuPipelineSeed seed = vu.capturePipeline();
    for (unsigned i = 1; i < 32; ++i) w.put<std::uint8_t>(seed.vfBusy[i]);
    w.put<std::uint32_t>(seed.head);
    w.put<std::uint8_t>(static_cast<std::uint8_t>(seed.flags.size()));
    for (const auto& f : seed.flags) {
        w.put<std::uint8_t>(f.delay);
        w.put<std::uint8_t>(f.hasMac ? 1 : 0);
        w.put<std::uint16_t>(f.mac);
        w.put<std::uint8_t>(f.hasClip ? 1 : 0);
        w.put<std::uint32_t>(f.clip);
    }
    for (const auto* u : {&seed.q, &seed.p}) {
        w.put<std::uint8_t>(u->active ? 1 : 0);
        w.put<std::uint8_t>(u->delay);
        w.put<std::uint32_t>(u->value);
        w.put<std::uint32_t>(u->flags);
    }
    w.raw(vu.data(), vu.dataSize());
    out_.write(reinterpret_cast<const char*>(w.buf.data()), static_cast<std::streamsize>(w.buf.size()));
    out_.flush();  // o jogo pode terminar por qualquer caminho
    ++entries_;
}

// ---------------------------------------------------------------------------
// Leitura
// ---------------------------------------------------------------------------

VuRecording VuRecording::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) throw anyps2::Error("não foi possível abrir a gravação do VU: " + path);
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(f.tellg()));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    Reader rd{buf, 0, path};

    char magic[4];
    rd.raw(magic, 4);
    if (std::memcmp(magic, kMagic, 4) != 0) throw anyps2::Error("não é uma gravação do VU (AVUR): " + path);
    const auto version = rd.get<std::uint32_t>();
    if (version != kVuRecVersion) {
        throw anyps2::Error("gravação do VU na versão " + std::to_string(version) + " (suportada: " +
                            std::to_string(kVuRecVersion) + "): " + path);
    }
    VuRecording rec;
    rec.microSize = rd.get<std::uint32_t>();
    rec.dataSize = rd.get<std::uint32_t>();
    rec.unit = rd.get<std::uint32_t>();
    if (rec.microSize == 0 || rec.dataSize == 0 || (rec.microSize & (rec.microSize - 1)) ||
        (rec.dataSize & (rec.dataSize - 1)) || rec.microSize > (1u << 20) || rec.dataSize > (1u << 20)) {
        throw anyps2::Error("gravação do VU com tamanhos inválidos: " + path);
    }

    std::map<std::uint64_t, std::shared_ptr<const std::vector<std::uint8_t>>> micros;
    while (!rd.atEnd()) {
        const auto type = rd.get<std::uint8_t>();
        if (type == kRecMicro) {
            const auto h = rd.get<std::uint64_t>();
            auto m = std::make_shared<std::vector<std::uint8_t>>(rec.microSize);
            rd.raw(m->data(), m->size());
            micros[h] = std::move(m);
        } else if (type == kRecEntry) {
            VuRecEntry e;
            e.vblank = rd.get<std::uint64_t>();
            e.startPc = rd.get<std::uint32_t>();
            e.eePc = rd.get<std::uint32_t>();
            e.microHash = rd.get<std::uint64_t>();
            e.vifTop = rd.get<std::uint32_t>();
            e.vifItop = rd.get<std::uint32_t>();
            e.cycle = rd.get<std::uint64_t>();
            rd.raw(e.vf, sizeof e.vf);
            rd.raw(e.vi, sizeof e.vi);
            rd.raw(&e.acc, sizeof e.acc);
            for (unsigned i = 1; i < 32; ++i) e.pipe.vfBusy[i] = rd.get<std::uint8_t>();
            e.pipe.head = rd.get<std::uint32_t>();
            const auto n = rd.get<std::uint8_t>();
            for (unsigned i = 0; i < n; ++i) {
                VuPipelineSeed::Flags fl;
                fl.delay = rd.get<std::uint8_t>();
                fl.hasMac = rd.get<std::uint8_t>() != 0;
                fl.mac = rd.get<std::uint16_t>();
                fl.hasClip = rd.get<std::uint8_t>() != 0;
                fl.clip = rd.get<std::uint32_t>();
                e.pipe.flags.push_back(fl);
            }
            for (auto* u : {&e.pipe.q, &e.pipe.p}) {
                u->active = rd.get<std::uint8_t>() != 0;
                u->delay = rd.get<std::uint8_t>();
                u->value = rd.get<std::uint32_t>();
                u->flags = rd.get<std::uint32_t>();
            }
            e.data.resize(rec.dataSize);
            rd.raw(e.data.data(), e.data.size());
            const auto it = micros.find(e.microHash);
            if (it == micros.end()) throw anyps2::Error("gravação do VU referencia uma imagem de micro ausente: " + path);
            e.micro = it->second;
            rec.entries.push_back(std::move(e));
        } else {
            throw anyps2::Error("gravação do VU com registro desconhecido (" + std::to_string(type) + "): " + path);
        }
    }
    return rec;
}

// ---------------------------------------------------------------------------
// Hash do estado final
// ---------------------------------------------------------------------------

std::uint64_t VuRunHash::all() const {
    Hasher h;
    for (const std::uint64_t v : {data, vec, ctl, pipe, kicks, pairs, cycles}) h.word(v);
    return h.done();
}

std::string VuRunHash::str() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%016llx %016llx %016llx %016llx %016llx %016llx %llu %llu",
                  static_cast<unsigned long long>(all()), static_cast<unsigned long long>(data),
                  static_cast<unsigned long long>(vec), static_cast<unsigned long long>(ctl),
                  static_cast<unsigned long long>(pipe), static_cast<unsigned long long>(kicks),
                  static_cast<unsigned long long>(pairs), static_cast<unsigned long long>(cycles));
    return buf;
}

bool VuRunHash::parse(const std::string& line, VuRunHash& out) {
    unsigned long long a, d, v, c, p, k, np, cy;
    if (std::sscanf(line.c_str(), "%llx %llx %llx %llx %llx %llx %llu %llu", &a, &d, &v, &c, &p, &k, &np, &cy) != 8)
        return false;
    out = {d, v, c, p, k, np, cy};
    return out.all() == a;
}

std::string VuRunHash::diff(const VuRunHash& o) const {
    std::string r;
    const auto add = [&](const char* name, bool differs) {
        if (!differs) return;
        if (!r.empty()) r += ",";
        r += name;
    };
    add("dados", data != o.data);
    add("vf/acc", vec != o.vec);
    add("vi/flags/Q/P/TPC", ctl != o.ctl);
    add("pipeline", pipe != o.pipe);
    add("xgkick", kicks != o.kicks);
    add("pares", pairs != o.pairs);
    add("ciclos", cycles != o.cycles);
    return r;
}

VuRunHash hashVuRun(Vu& vu, std::uint64_t cycle0, const std::vector<VuXgkickEvent>& kicks) {
    VuRunHash r;
    const vucore::Regs& regs = vu.regs();
    r.data = hashBytes(vu.data(), vu.dataSize());
    {
        Hasher h;
        h.bytes(regs.vf, 32 * sizeof(Reg128));
        h.bytes(regs.acc, sizeof(Reg128));
        r.vec = h.done();
    }
    r.ctl = hashBytes(regs.vi, 32 * sizeof(std::uint32_t));
    {
        const VuPipelineSeed s = vu.capturePipeline();
        Hasher h;
        h.bytes(s.vfBusy.data(), s.vfBusy.size());
        h.word(s.head);
        h.word(s.flags.size());
        for (const auto& f : s.flags) {
            h.word(f.delay | (std::uint64_t{f.hasMac} << 8) | (std::uint64_t{f.hasClip} << 9) |
                   (std::uint64_t{f.mac} << 16) | (std::uint64_t{f.clip} << 32));
        }
        for (const auto* u : {&s.q, &s.p}) {
            h.word(std::uint64_t{u->active} | (std::uint64_t{u->delay} << 8) | (std::uint64_t{u->value} << 32));
            h.word(u->flags);
        }
        r.pipe = h.done();
    }
    {
        Hasher h;
        h.word(kicks.size());
        for (const auto& k : kicks) {
            h.word(k.addr);
            h.word(k.cycle - cycle0);
        }
        r.kicks = h.done();
    }
    r.pairs = vu.pairs();
    r.cycles = vu.cycle() - cycle0;
    return r;
}

// ---------------------------------------------------------------------------
// Replay
// ---------------------------------------------------------------------------

VuReplayMachine::VuReplayMachine(std::uint32_t microSize, std::uint32_t dataSize, unsigned unit)
    : data_(dataSize),
      micro_(microSize),
      vu_(nullptr, unit, vucore::Regs{vf_, vi_, &acc_}, data_.data(), dataSize, micro_.data(), microSize) {
    vu_.setXgkickLog(&kicks_);
}

void VuReplayMachine::apply(const VuRecEntry& e) {
    if (lastMicro_ != e.micro.get()) {  // a imagem só é recopiada quando muda
        std::memcpy(micro_.data(), e.micro->data(), micro_.size());
        lastMicro_ = e.micro.get();
    }
    std::memcpy(data_.data(), e.data.data(), data_.size());
    std::memcpy(vf_, e.vf, sizeof vf_);
    std::memcpy(vi_, e.vi, sizeof vi_);
    acc_ = e.acc;
    vu_.setVifTop(e.vifTop, e.vifItop);
    vu_.seedPipeline(e.pipe);
}

VuRunHash VuReplayMachine::run(const VuRecEntry& e, bool interpret, bool hash, std::uint64_t* ns, std::string* error) {
    kicks_.clear();
    const std::uint64_t c0 = vu_.cycle();
    const auto t0 = std::chrono::steady_clock::now();
    try {
        if (interpret) vu_.interpret(e.startPc, e.eePc);
        else vu_.start(e.startPc, e.eePc);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
    }
    if (ns) {
        *ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    }
    return hash ? hashVuRun(vu_, c0, kicks_) : VuRunHash{};
}

}  // namespace anyps2::rt
