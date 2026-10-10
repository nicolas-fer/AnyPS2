// Diagnóstico do GS (ANYPS2_GS_PROBE e ANYPS2_GS_DRAWLOG); ver gs_trace.h.
//
// As coordenadas dos pontos são lidas com as mesmas contas do rasterizador
// (gs_coverage e a interpolação de gs_draw.cpp), então o que se registra para
// cada pixel é o que o desenho de fato escreveu, não uma aproximação.

#include "anyps2/runtime/gs/gs_trace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ostream>
#include <string>

#include "anyps2/runtime/gs/vram.h"

namespace anyps2::rt::gs {

namespace {

constexpr std::uint64_t bits(std::uint64_t v, unsigned lo, unsigned n) {
    return (v >> lo) & ((1ull << n) - 1);
}

std::string hex(std::uint64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%0*llX", digits, static_cast<unsigned long long>(v));
    return buf;
}

std::string fixed(double v) {
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

void warn(const std::string& what) {
    std::fprintf(stderr, "anyps2: aviso: GS (diagnóstico): %s\n", what.c_str());
}

const char* primName(unsigned type) {
    switch (type) {
        case 0: return "ponto";
        case 1: return "linha";
        case 2: return "linha-strip";
        case 3: return "triângulo";
        case 4: return "triângulo-strip";
        case 5: return "triângulo-fan";
        case 6: return "sprite";
        default: return "inválido";
    }
}

// Vértices que cada tipo de primitiva usa.
unsigned vertexCount(unsigned type) {
    static constexpr unsigned kCount[7] = {1, 2, 2, 3, 3, 3, 2};
    return type < 7 ? kCount[type] : 0;
}

// Mesma regra de covers() em gs_coverage.cpp: SCISSOR (inclusivo) e SCANMSK.
bool inWindow(const DrawWindow& w, int x, int y) {
    if (x < w.x0 || x > w.x1 || y < w.y0 || y > w.y1) return false;
    if ((w.scanmsk == 2 && (y & 1)) || (w.scanmsk == 3 && !(y & 1))) return false;
    return true;
}

// O pixel (px, py) está na caixa do desenho? Pontos e linhas: pixel exato que o
// desenho escreveria. Triângulos e sprites: caixa depois do SCISSOR.
bool hitsPoint(unsigned type, const Vertex* v, const DrawWindow& w, int px, int py) {
    switch (type) {
        case 0: {
            const int x = (v[0].x + 8) >> 4, y = (v[0].y + 8) >> 4;
            return x == px && y == py && inWindow(w, x, y);
        }
        case 1:
        case 2: {
            const int steps = lineSteps(v[0], v[1]);
            for (int i = 0; i < std::max(steps, 1); ++i) {
                const LineSample s = lineSample(v[0], v[1], steps, i);
                if (s.px == px && s.py == py && inWindow(w, px, py)) return true;
            }
            return false;
        }
        case 3:
        case 4:
        case 5: {
            const TriangleSetup t = triangleSetup(w, v[0], v[1], v[2]);
            return t.valid && px >= t.minX && px <= t.maxX && py >= t.minY && py <= t.maxY;
        }
        case 6: {
            const SpriteRect r = spriteRect(w, v[0], v[1]);
            return !r.empty() && px >= r.minX && px <= r.maxX && py >= r.minY && py <= r.maxY;
        }
        default:
            return false;
    }
}

// Coordenadas de textura no pixel, interpoladas como o rasterizador faz: linear
// no sprite e nas linhas, baricêntrica (com a mesma função de aresta) no triângulo.
TexCoords texAt(unsigned type, const Vertex* v, const DrawWindow& w, int x, int y) {
    TexCoords c;
    switch (type) {
        case 0:
            c.s = v[0].s;
            c.t = v[0].t;
            c.q = v[0].q;
            c.u = static_cast<double>(v[0].u) / 16.0;
            c.v = static_cast<double>(v[0].v) / 16.0;
            break;
        case 1:
        case 2: {
            const int steps = lineSteps(v[0], v[1]);
            for (int i = 0; i < std::max(steps, 1); ++i) {
                const LineSample s = lineSample(v[0], v[1], steps, i);
                if (s.px != x || s.py != y) continue;
                const double t = s.t;
                const double a0u = static_cast<double>(v[0].u) / 16.0, a1u = static_cast<double>(v[1].u) / 16.0;
                const double a0v = static_cast<double>(v[0].v) / 16.0, a1v = static_cast<double>(v[1].v) / 16.0;
                c.s = v[0].s * (1 - t) + v[1].s * t;
                c.t = v[0].t * (1 - t) + v[1].t * t;
                c.q = v[0].q * (1 - t) + v[1].q * t;
                c.u = a0u * (1 - t) + a1u * t;
                c.v = a0v * (1 - t) + a1v * t;
                break;
            }
            break;
        }
        case 3:
        case 4:
        case 5: {
            const TriangleSetup t = triangleSetup(w, v[0], v[1], v[2]);
            if (!t.valid) break;
            const double inv = 1.0 / static_cast<double>(t.area);
            const std::int64_t dx = std::int64_t{x} - t.minX, dy = std::int64_t{y} - t.minY;
            double wt[3];
            for (int k = 0; k < 3; ++k) {
                wt[k] = static_cast<double>(t.row[k] + dx * t.dx[k] + dy * t.dy[k]) * inv;
            }
            const Vertex& A = t.v[0];
            const Vertex& B = t.v[1];
            const Vertex& C = t.v[2];
            auto mix = [&](double a, double b, double cc) { return a * wt[0] + b * wt[1] + cc * wt[2]; };
            c.s = mix(A.s, B.s, C.s);
            c.t = mix(A.t, B.t, C.t);
            c.q = mix(A.q, B.q, C.q);
            c.u = mix(static_cast<double>(A.u), static_cast<double>(B.u), static_cast<double>(C.u)) / 16.0;
            c.v = mix(static_cast<double>(A.v), static_cast<double>(B.v), static_cast<double>(C.v)) / 16.0;
            break;
        }
        case 6: {
            // Mesma conta de drawSprite: S/U pela coluna, T/V pela linha, Q do segundo vértice.
            const double spanX = v[1].x - v[0].x, spanY = v[1].y - v[0].y;
            const double tx = spanX != 0 ? (x * 16.0 - v[0].x) / spanX : 0.0;
            const double ty = spanY != 0 ? (y * 16.0 - v[0].y) / spanY : 0.0;
            const double a0u = static_cast<double>(v[0].u) / 16.0, a1u = static_cast<double>(v[1].u) / 16.0;
            const double a0v = static_cast<double>(v[0].v) / 16.0, a1v = static_cast<double>(v[1].v) / 16.0;
            c.s = v[0].s + (v[1].s - v[0].s) * tx;
            c.t = v[0].t + (v[1].t - v[0].t) * ty;
            c.q = v[1].q;
            c.u = a0u + (a1u - a0u) * tx;
            c.v = a0v + (a1v - a0v) * ty;
            break;
        }
        default:
            break;
    }
    return c;
}

// Cor como a VRAM a guarda no formato do FRAME. Em 16 bits, o bit 15 é o alfa.
std::string colorText(std::uint32_t raw, std::uint32_t psm) {
    std::uint64_t r = 0, g = 0, b = 0, a = 0;
    bool hasAlpha = true;
    if (psm == PSMCT16 || psm == PSMCT16S) {
        r = (raw & 0x1F) << 3;
        g = ((raw >> 5) & 0x1F) << 3;
        b = ((raw >> 10) & 0x1F) << 3;
        a = (raw & 0x8000) ? 0x80 : 0;
    } else {
        r = raw & 0xFF;
        g = (raw >> 8) & 0xFF;
        b = (raw >> 16) & 0xFF;
        a = raw >> 24;
        if (psm == PSMCT24 || psm == PSMZ24) hasAlpha = false;
    }
    std::string out = "rgba(" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(b) + ",";
    out += hasAlpha ? std::to_string(a) : std::string("-");
    return out + ")  raw=" + hex(raw, 8);
}

std::string zText(bool valid, std::uint32_t z) {
    if (!valid) return "sem Z válido";
    return std::to_string(z) + " (" + hex(z, 8) + ")";
}

void writeVertex(std::ostream& o, unsigned i, const Vertex& v) {
    o << "  v" << i << ": x=" << fixed(v.x / 16.0) << " y=" << fixed(v.y / 16.0) << " z=" << v.z
      << " RGBAQ=(" << static_cast<unsigned>(v.r) << "," << static_cast<unsigned>(v.g) << ","
      << static_cast<unsigned>(v.b) << "," << static_cast<unsigned>(v.a) << ") ST=(" << fixed(v.s) << ","
      << fixed(v.t) << ") Q=" << fixed(v.q) << " UV=(" << fixed(v.u / 16.0) << "," << fixed(v.v / 16.0)
      << ") F=" << static_cast<unsigned>(v.fog) << "\n";
}

// Estado do desenho, em campos (o mesmo que a sonda grava).
void writeState(std::ostream& o, const DrawState& s) {
    const std::uint64_t fbp = bits(s.frame, 0, 9);
    const auto fpsm = static_cast<std::uint32_t>(bits(s.frame, 24, 6));
    const auto zpsm = static_cast<std::uint32_t>(0x30u | bits(s.zbuf, 24, 4));
    o << "  PRIM=" << hex(s.prim, 3) << " tipo=" << bits(s.prim, 0, 3) << " IIP=" << bits(s.attr, 3, 1)
      << " TME=" << bits(s.attr, 4, 1) << " FGE=" << bits(s.attr, 5, 1) << " ABE=" << bits(s.attr, 6, 1)
      << " AA1=" << bits(s.attr, 7, 1) << " FST=" << bits(s.attr, 8, 1) << " CTXT=" << s.ctxt
      << " (atributo de " << (s.useprim ? "PRIM" : "PRMODE") << ")\n";
    o << "  FRAME: FBP=" << hex(fbp, 3) << " (bloco " << hex(fbp * 32, 4) << ") FBW=" << bits(s.frame, 16, 6)
      << " PSM=" << psmName(fpsm) << " FBMSK=" << hex(s.frame >> 32, 8) << "\n";
    o << "  ZBUF: ZBP=" << hex(bits(s.zbuf, 0, 9), 3) << " (bloco " << hex(bits(s.zbuf, 0, 9) * 32, 4)
      << ") PSM=" << psmName(zpsm) << " ZMSK=" << bits(s.zbuf, 32, 1) << "\n";
    o << "  TEST: ATE=" << bits(s.test, 0, 1) << " ATST=" << bits(s.test, 1, 3) << " AREF=" << bits(s.test, 4, 8)
      << " AFAIL=" << bits(s.test, 12, 2) << " DATE=" << bits(s.test, 14, 1) << " DATM=" << bits(s.test, 15, 1)
      << " ZTE=" << bits(s.test, 16, 1) << " ZTST=" << bits(s.test, 17, 2) << "\n";
    o << "  ALPHA: A=" << bits(s.alpha, 0, 2) << " B=" << bits(s.alpha, 2, 2) << " C=" << bits(s.alpha, 4, 2)
      << " D=" << bits(s.alpha, 6, 2) << " FIX=" << hex(bits(s.alpha, 32, 8), 2) << " | FBA=" << bits(s.fba, 0, 1)
      << " PABE=" << bits(s.pabe, 0, 1) << " DTHE=" << bits(s.dthe, 0, 1) << " COLCLAMP=" << bits(s.colclamp, 0, 1)
      << "\n";
    o << "  TEX0: TBP0=" << hex(bits(s.tex0, 0, 14), 4) << " TBW=" << bits(s.tex0, 14, 6)
      << " PSM=" << psmName(static_cast<std::uint32_t>(bits(s.tex0, 20, 6))) << " TW=" << bits(s.tex0, 26, 4)
      << " TH=" << bits(s.tex0, 30, 4) << " TCC=" << bits(s.tex0, 34, 1) << " TFX=" << bits(s.tex0, 35, 2)
      << " CBP=" << hex(bits(s.tex0, 37, 14), 4) << " CPSM=" << psmName(static_cast<std::uint32_t>(bits(s.tex0, 51, 4)))
      << " CSM=" << bits(s.tex0, 55, 1) << " CSA=" << bits(s.tex0, 56, 5) << "\n";
    o << "  TEX1: LCM=" << bits(s.tex1, 0, 1) << " MXL=" << bits(s.tex1, 2, 3) << " MMAG=" << bits(s.tex1, 5, 1)
      << " MMIN=" << bits(s.tex1, 6, 3) << " MTBA=" << bits(s.tex1, 9, 1) << " L=" << bits(s.tex1, 19, 2)
      << " K=" << bits(s.tex1, 32, 12) << "\n";
    o << "  CLAMP: WMS=" << bits(s.clamp, 0, 2) << " WMT=" << bits(s.clamp, 2, 2) << " MINU=" << bits(s.clamp, 4, 10)
      << " MAXU=" << bits(s.clamp, 14, 10) << " MINV=" << bits(s.clamp, 24, 10) << " MAXV=" << bits(s.clamp, 34, 10)
      << "\n";
    o << "  TEXA: TA0=" << bits(s.texa, 0, 8) << " AEM=" << bits(s.texa, 15, 1) << " TA1=" << bits(s.texa, 32, 8)
      << "\n";
    o << "  SCISSOR: X0=" << bits(s.scissor, 0, 11) << " X1=" << bits(s.scissor, 16, 11)
      << " Y0=" << bits(s.scissor, 32, 11) << " Y1=" << bits(s.scissor, 48, 11) << " SCANMSK=" << s.scanmsk << "\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Configuração
// ---------------------------------------------------------------------------

namespace {

bool parseU64(const char* s, std::uint64_t& out) {
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 0);
    if (end == s || *end != 0) return false;
    out = static_cast<std::uint64_t>(v);
    return true;
}

// "x,y[,x2,y2...]" com até GsTrace::kMaxPoints pontos, coordenadas não negativas.
bool parsePoints(const char* s, std::vector<std::pair<int, int>>& out) {
    std::vector<long> nums;
    const char* p = s;
    while (*p) {
        char* end = nullptr;
        const long v = std::strtol(p, &end, 10);
        if (end == p || v < 0) return false;
        nums.push_back(v);
        p = end;
        if (*p == ',') {
            ++p;
        } else if (*p != 0) {
            return false;
        }
    }
    if (nums.empty() || nums.size() % 2 != 0 || nums.size() / 2 > GsTrace::kMaxPoints) return false;
    for (std::size_t i = 0; i < nums.size(); i += 2) {
        out.emplace_back(static_cast<int>(nums[i]), static_cast<int>(nums[i + 1]));
    }
    return true;
}

}  // namespace

std::unique_ptr<GsTrace> GsTrace::fromEnv() {
    const char* probe = std::getenv("ANYPS2_GS_PROBE");
    const char* log = std::getenv("ANYPS2_GS_DRAWLOG");
    if (probe && !*probe) probe = nullptr;
    if (log && !*log) log = nullptr;
    if (!probe && !log) return nullptr;

    GsTraceConfig cfg;
    if (probe && !parsePoints(probe, cfg.points)) {
        warn("ANYPS2_GS_PROBE espera x,y[,x2,y2...] com até 8 pontos; sonda desligada");
        cfg.points.clear();
    }
    if (const char* v = std::getenv("ANYPS2_GS_PROBE_FROM")) {
        if (!parseU64(v, cfg.probeFrom)) warn("ANYPS2_GS_PROBE_FROM inválido; usando 0");
    }
    if (const char* v = std::getenv("ANYPS2_GS_PROBE_TO")) {
        if (!parseU64(v, cfg.probeTo)) warn("ANYPS2_GS_PROBE_TO inválido; sem limite superior");
    }
    if (const char* v = std::getenv("ANYPS2_GS_PROBE_FBP")) {
        std::uint64_t fbp = 0;
        if (parseU64(v, fbp) && fbp <= 0x1FF) {
            cfg.filterFbp = true;
            cfg.fbp = static_cast<std::uint32_t>(fbp);
        } else {
            warn("ANYPS2_GS_PROBE_FBP inválido (campo FBP do FRAME, 0..511); aceitando qualquer buffer");
        }
    }
    if (const char* v = std::getenv("ANYPS2_GS_PROBE_OUT")) {
        if (*v) cfg.probeOut = v;
    }
    if (log) cfg.drawLog = log;
    if (const char* v = std::getenv("ANYPS2_GS_DRAWLOG_FROM")) {
        if (!parseU64(v, cfg.drawFrom)) warn("ANYPS2_GS_DRAWLOG_FROM inválido; usando 0");
    }
    if (const char* v = std::getenv("ANYPS2_GS_DRAWLOG_TO")) {
        if (!parseU64(v, cfg.drawTo)) warn("ANYPS2_GS_DRAWLOG_TO inválido; sem limite superior");
    }

    auto trace = std::make_unique<GsTrace>();
    trace->configure(cfg);
    if (!trace->active()) return nullptr;
    return trace;
}

void GsTrace::configure(const GsTraceConfig& cfg) {
    probe_.close();
    log_.close();
    points_.clear();
    pending_ = Pending{};

    if (cfg.points.size() > kMaxPoints) warn("a sonda aceita até 8 pontos; usando os 8 primeiros");
    const std::size_t n = std::min(cfg.points.size(), kMaxPoints);
    points_.assign(cfg.points.begin(), cfg.points.begin() + static_cast<std::ptrdiff_t>(n));
    if (!points_.empty()) {
        probe_.open(cfg.probeOut, std::ios::out | std::ios::trunc);
        if (!probe_.is_open()) {
            warn("não consegui abrir " + cfg.probeOut + "; sonda desligada");
            points_.clear();
        }
    }
    probeFrom_ = cfg.probeFrom;
    probeTo_ = cfg.probeTo;
    filterFbp_ = cfg.filterFbp;
    fbp_ = cfg.fbp;

    if (!cfg.drawLog.empty()) {
        log_.open(cfg.drawLog, std::ios::out | std::ios::trunc);
        if (!log_.is_open()) warn("não consegui abrir " + cfg.drawLog + "; log de desenhos desligado");
    }
    drawFrom_ = cfg.drawFrom;
    drawTo_ = cfg.drawTo;
}

// ---------------------------------------------------------------------------
// Ganchos de Gs::draw
// ---------------------------------------------------------------------------

DrawState GsTrace::capture(const Gs& gs) {
    DrawState s;
    s.prim = gs.regs_[PRIM];
    s.useprim = (gs.regs_[PRMODECONT] & 1) != 0;
    s.attr = s.useprim ? s.prim : gs.regs_[PRMODE];
    s.ctxt = static_cast<unsigned>(bits(s.attr, 9, 1));
    const Gs::Context& k = gs.ctx_[s.ctxt];
    s.frame = k.frame;
    s.zbuf = k.zbuf;
    s.test = k.test;
    s.alpha = k.alpha;
    s.scissor = k.scissor;
    s.tex0 = k.tex0;
    s.tex1 = k.tex1;
    s.clamp = k.clamp;
    s.fba = k.fba;
    s.texa = gs.regs_[TEXA];
    s.pabe = gs.regs_[PABE];
    s.dthe = gs.regs_[DTHE];
    s.colclamp = gs.regs_[COLCLAMP];
    s.scanmsk = gs.regs_[SCANMSK];
    return s;
}

bool GsTrace::beginDraw(Gs& gs, std::uint32_t pc, unsigned type, const DrawWindow& w, const Vertex& v0,
                        const Vertex& v1, const Vertex& v2) {
    pending_.active = false;
    const std::uint64_t vb = gs.vblanks_;
    const bool logging = log_.is_open() && vb >= drawFrom_ && vb <= drawTo_;
    const bool probing = !points_.empty() && vb >= probeFrom_ && vb <= probeTo_;
    if (!logging && !probing) return false;

    const DrawState st = capture(gs);
    if (logging) writeLogLine(vb, gs.drawCount_, pc, type, st);
    if (!probing) return false;
    if (filterFbp_ && bits(st.frame, 0, 9) != fbp_) return false;

    const Vertex v[3] = {v0, v1, v2};
    std::vector<Hit> hits;
    for (const auto& p : points_) {
        if (!hitsPoint(type, v, w, p.first, p.second)) continue;
        Hit h;
        h.x = p.first;
        h.y = p.second;
        h.at = texAt(type, v, w, h.x, h.y);
        hits.push_back(h);
    }
    if (hits.empty()) return false;

    // Antes do desenho: a VRAM tem de refletir todos os desenhos anteriores.
    gs.waitIdle();
    const auto fbp = static_cast<std::uint32_t>(bits(st.frame, 0, 9) * 32);
    const auto fbw = static_cast<std::uint32_t>(bits(st.frame, 16, 6));
    const auto fpsm = static_cast<std::uint32_t>(bits(st.frame, 24, 6));
    const auto zbp = static_cast<std::uint32_t>(bits(st.zbuf, 0, 9) * 32);
    const auto zpsm = static_cast<std::uint32_t>(0x30u | bits(st.zbuf, 24, 4));
    const bool zValid = isValidPsm(zpsm);
    for (Hit& h : hits) {
        const auto x = static_cast<std::uint32_t>(h.x), y = static_cast<std::uint32_t>(h.y);
        h.colorBefore = gs.vram_.readPixel(fpsm, fbp, fbw, x, y);
        if (zValid) h.zBefore = gs.vram_.readPixel(zpsm, zbp, fbw, x, y);
    }

    pending_.active = true;
    pending_.vblank = vb;
    pending_.draw = gs.drawCount_;
    pending_.pc = pc;
    pending_.type = type;
    for (unsigned i = 0; i < 3; ++i) pending_.v[i] = v[i];
    pending_.state = st;
    pending_.hits = std::move(hits);
    return true;
}

void GsTrace::endDraw(Gs& gs) {
    if (!pending_.active) return;
    // Depois do desenho: espera o worker terminar e lê o que ficou na VRAM.
    gs.waitIdle();
    const DrawState& st = pending_.state;
    const auto fbp = static_cast<std::uint32_t>(bits(st.frame, 0, 9) * 32);
    const auto fbw = static_cast<std::uint32_t>(bits(st.frame, 16, 6));
    const auto fpsm = static_cast<std::uint32_t>(bits(st.frame, 24, 6));
    const auto zbp = static_cast<std::uint32_t>(bits(st.zbuf, 0, 9) * 32);
    const auto zpsm = static_cast<std::uint32_t>(0x30u | bits(st.zbuf, 24, 4));
    const bool zValid = isValidPsm(zpsm);
    for (Hit& h : pending_.hits) {
        const auto x = static_cast<std::uint32_t>(h.x), y = static_cast<std::uint32_t>(h.y);
        h.colorAfter = gs.vram_.readPixel(fpsm, fbp, fbw, x, y);
        if (zValid) h.zAfter = gs.vram_.readPixel(zpsm, zbp, fbw, x, y);
    }
    writeProbe();
    pending_.active = false;
}

// ---------------------------------------------------------------------------
// Saídas
// ---------------------------------------------------------------------------

void GsTrace::writeLogLine(std::uint64_t vblank, std::uint64_t draw, std::uint32_t pc, unsigned type,
                           const DrawState& s) {
    const auto fpsm = static_cast<std::uint32_t>(bits(s.frame, 24, 6));
    const auto zpsm = static_cast<std::uint32_t>(0x30u | bits(s.zbuf, 24, 4));
    log_ << "vb=" << vblank << " desenho=" << draw << " pc=" << hex(pc, 8) << " tipo=" << primName(type)
         << " ctx=" << s.ctxt << " IIP=" << bits(s.attr, 3, 1) << " TME=" << bits(s.attr, 4, 1)
         << " FGE=" << bits(s.attr, 5, 1) << " ABE=" << bits(s.attr, 6, 1) << " FST=" << bits(s.attr, 8, 1)
         << " FB=" << hex(bits(s.frame, 0, 9), 3) << " FBW=" << bits(s.frame, 16, 6) << " PSM=" << psmName(fpsm)
         << " MSK=" << hex(s.frame >> 32, 8) << " ZB=" << hex(bits(s.zbuf, 0, 9), 3) << " ZPSM=" << psmName(zpsm)
         << " ZMSK=" << bits(s.zbuf, 32, 1) << " ATE=" << bits(s.test, 0, 1) << " ATST=" << bits(s.test, 1, 3)
         << " AREF=" << bits(s.test, 4, 8) << " AFAIL=" << bits(s.test, 12, 2) << " DATE=" << bits(s.test, 14, 1)
         << " DATM=" << bits(s.test, 15, 1) << " ZTE=" << bits(s.test, 16, 1) << " ZTST=" << bits(s.test, 17, 2)
         << " ALPHA=" << bits(s.alpha, 0, 2) << "," << bits(s.alpha, 2, 2) << "," << bits(s.alpha, 4, 2) << ","
         << bits(s.alpha, 6, 2) << "," << hex(bits(s.alpha, 32, 8), 2) << " TBP0=" << hex(bits(s.tex0, 0, 14), 4)
         << " TPSM=" << psmName(static_cast<std::uint32_t>(bits(s.tex0, 20, 6))) << " TW=" << bits(s.tex0, 26, 4)
         << " TH=" << bits(s.tex0, 30, 4) << " TFX=" << bits(s.tex0, 35, 2) << "\n";
}

void GsTrace::writeProbe() {
    const Pending& p = pending_;
    const DrawState& st = p.state;
    const auto fpsm = static_cast<std::uint32_t>(bits(st.frame, 24, 6));
    const auto zpsm = static_cast<std::uint32_t>(0x30u | bits(st.zbuf, 24, 4));
    const bool zValid = isValidPsm(zpsm);
    const std::uint64_t tw = std::min<std::uint64_t>(bits(st.tex0, 26, 4), 10);
    const std::uint64_t th = std::min<std::uint64_t>(bits(st.tex0, 30, 4), 10);

    std::ostream& o = probe_;
    o << "=== VBlank " << p.vblank << " | desenho " << p.draw << " | pc " << hex(p.pc, 8) << " | "
      << primName(p.type) << " (tipo " << p.type << ")\n";
    writeState(o, st);
    o << "  vértices (pixels, sem XYOFFSET):\n";
    for (unsigned i = 0; i < vertexCount(p.type); ++i) writeVertex(o, i, p.v[i]);
    for (const Hit& h : p.hits) {
        o << "PONTO (" << h.x << ", " << h.y << "):\n";
        o << "  cor antes:  " << colorText(h.colorBefore, fpsm) << "\n";
        o << "  cor depois: " << colorText(h.colorAfter, fpsm) << "\n";
        o << "  Z antes:  " << zText(zValid, h.zBefore) << "\n";
        o << "  Z depois: " << zText(zValid, h.zAfter) << "\n";
        o << "  ST/Q interpolados: s=" << fixed(h.at.s) << " t=" << fixed(h.at.t) << " q=" << fixed(h.at.q)
          << "  UV: u=" << fixed(h.at.u) << " v=" << fixed(h.at.v) << "\n";
        // Texel no nível 0: STQ divide por Q; com FST=1 o UV já está em texels.
        const bool fst = bits(st.attr, 8, 1) != 0;
        if (!fst && h.at.q == 0) {
            o << "  texel no nível 0: indefinido (Q = 0)\n";
        } else {
            const double texU = fst ? h.at.u : h.at.s / h.at.q * static_cast<double>(1ull << tw);
            const double texV = fst ? h.at.v : h.at.t / h.at.q * static_cast<double>(1ull << th);
            o << "  texel no nível 0: u=" << fixed(texU) << " v=" << fixed(texV)
              << "  (texel e CLUT amostrados: pendente, exige o amostrador do rasterizador)\n";
        }
    }
    o << "\n";
    o.flush();
}

}  // namespace anyps2::rt::gs
