#include "anyps2/analysis/analyzer.h"

#include <algorithm>
#include <map>
#include <utility>

#include "anyps2/common/error.h"
#include "anyps2/r5900/decoder.h"

namespace anyps2::analysis {

using r5900::Instruction;
using r5900::Op;

namespace {

std::string subName(std::uint32_t a) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "sub_%08X", a);
    return buf;
}

std::vector<CodeRegion> findRegions(const elf::ElfFile& elf) {
    std::vector<CodeRegion> regions;
    for (const auto& s : elf.sections()) {
        if (s.isAlloc() && s.isExec() && s.type == elf::SHT_PROGBITS && s.size >= 4) {
            regions.push_back({s.addr, s.addr + (s.size & ~3u), s.name});
        }
    }
    if (regions.empty()) {
        for (const auto& s : elf.segments()) {
            if (s.isLoad() && s.executable() && s.filesz >= 4) {
                regions.push_back({s.vaddr, s.vaddr + (s.filesz & ~3u),
                                   "segmento " + std::to_string(s.index)});
            }
        }
    }
    std::sort(regions.begin(), regions.end(),
              [](const CodeRegion& a, const CodeRegion& b) { return a.start < b.start; });
    return regions;
}

const CodeRegion* regionOf(const std::vector<CodeRegion>& regions, std::uint32_t a) {
    for (const auto& r : regions) {
        if (a >= r.start && a < r.end) return &r;
    }
    return nullptr;
}

}  // namespace

const Function* ProgramModel::functionContaining(std::uint32_t a) const {
    auto it = std::upper_bound(functions.begin(), functions.end(), a,
                               [](std::uint32_t v, const Function& f) { return v < f.start; });
    if (it == functions.begin()) return nullptr;
    --it;
    return it->contains(a) ? &*it : nullptr;
}

Function* ProgramModel::functionContaining(std::uint32_t a) {
    return const_cast<Function*>(std::as_const(*this).functionContaining(a));
}

bool ProgramModel::inCode(std::uint32_t a) const {
    return regionOf(regions, a) != nullptr;
}

ProgramModel analyze(const elf::ElfFile& elf, const AnalysisOptions& options) {
    ProgramModel model;
    model.entry = elf.entry();
    model.regions = findRegions(elf);
    if (model.regions.empty()) {
        throw anyps2::Error(elf.name() + ": nenhuma seção ou segmento executável encontrado");
    }
    auto inCode = [&](std::uint32_t a) { return (a & 3) == 0 && regionOf(model.regions, a); };
    auto word = [&](std::uint32_t a) { return elf.read32(a); };

    // ---- 1. Inícios de função -------------------------------------------
    std::map<std::uint32_t, std::pair<std::string, std::uint32_t>> starts;  // addr -> (nome, tamanho)
    auto addStart = [&](std::uint32_t a, const std::string& name, std::uint32_t size) {
        if (!inCode(a)) return;
        auto it = starts.find(a);
        if (it == starts.end()) {
            starts.emplace(a, std::make_pair(name, size));
        } else if (it->second.first.rfind("sub_", 0) == 0 && name.rfind("sub_", 0) != 0) {
            it->second = {name, size};
        } else if (it->second.second == 0 && size != 0) {
            it->second.second = size;
        }
    };
    if (inCode(model.entry)) addStart(model.entry, subName(model.entry), 0);
    // Símbolos de função (prefere globais para o nome).
    std::vector<const elf::Symbol*> funcSyms;
    for (const auto& s : elf.symbols()) {
        if (s.isFunction() && s.isDefined() && !s.name.empty()) funcSyms.push_back(&s);
    }
    std::stable_sort(funcSyms.begin(), funcSyms.end(), [](const elf::Symbol* a, const elf::Symbol* b) {
        return (a->bind != elf::STB_LOCAL) > (b->bind != elf::STB_LOCAL);
    });
    for (const auto* s : funcSyms) addStart(s->value, s->name, s->size);
    for (auto a : options.extraFunctions) addStart(a, subName(a), 0);
    // Alvos de JAL.
    for (const auto& r : model.regions) {
        for (std::uint32_t a = r.start; a < r.end; a += 4) {
            const Instruction insn = r5900::decode(word(a), a);
            if (insn.op == Op::JAL && inCode(insn.jumpTarget())) {
                addStart(insn.jumpTarget(), subName(insn.jumpTarget()), 0);
            }
        }
    }

    // ---- 2. Faixas -------------------------------------------------------
    std::vector<std::uint32_t> startList;
    for (const auto& [a, info] : starts) startList.push_back(a);
    for (std::size_t i = 0; i < startList.size(); ++i) {
        const std::uint32_t s = startList[i];
        const CodeRegion* region = regionOf(model.regions, s);
        if (!region) continue;  // addStart só aceita endereços dentro de código
        std::uint32_t end = region->end;
        if (i + 1 < startList.size() && startList[i + 1] < end) end = startList[i + 1];
        const auto& [name, size] = starts[s];
        if (size != 0 && s + size < end) end = s + ((size + 3) & ~3u);
        Function f;
        f.start = s;
        f.end = end;
        f.name = name;
        f.fromSymbol = name.rfind("sub_", 0) != 0;
        model.functions.push_back(std::move(f));
    }
    // Lacunas não cobertas (código sem símbolo): ignora padding de zeros.
    std::vector<Function> gaps;
    for (const auto& r : model.regions) {
        std::uint32_t cursor = r.start;
        auto flushGap = [&](std::uint32_t gapEnd) {
            std::uint32_t a = cursor;
            while (a < gapEnd && word(a) == 0) a += 4;
            if (a < gapEnd) {
                Function f;
                f.start = a;
                f.end = gapEnd;
                f.name = subName(a);
                gaps.push_back(std::move(f));
            }
        };
        for (const auto& f : model.functions) {
            if (f.start < r.start || f.start >= r.end) continue;
            if (f.start > cursor) flushGap(f.start);
            cursor = std::max(cursor, f.end);
        }
        if (cursor < r.end) flushGap(r.end);
    }
    for (auto& g : gaps) model.functions.push_back(std::move(g));
    std::sort(model.functions.begin(), model.functions.end(),
              [](const Function& a, const Function& b) { return a.start < b.start; });

    // ---- 3. Rótulos, retornos e alvos externos --------------------------
    std::vector<std::pair<std::uint32_t, std::uint32_t>> external;  // (origem, alvo)
    for (auto& f : model.functions) {
        for (std::uint32_t a = f.start; a < f.end; a += 4) {
            const Instruction insn = r5900::decode(word(a), a);
            if (!insn.valid()) continue;
            if (insn.hasDelaySlot() && a + 4 < f.end) {
                const Instruction slot = r5900::decode(word(a + 4), a + 4);
                if (slot.hasDelaySlot()) {
                    model.warnings.push_back({a + 4, "desvio dentro de delay slot (comportamento indefinido)"});
                }
            }
            if (insn.isBranch() || insn.op == Op::J) {
                const std::uint32_t t = insn.staticTarget();
                const bool call = insn.isLink() && t == f.start;  // bal para o próprio início
                if (f.contains(t) && !call) f.labels.insert(t);
                else external.emplace_back(a, t);
                if (insn.isLink() && a + 8 < f.end) f.entries.insert(a + 8);
            } else if (insn.op == Op::JAL) {
                external.emplace_back(a, insn.jumpTarget());
                if (a + 8 < f.end) f.entries.insert(a + 8);
            } else if (insn.op == Op::JALR) {
                if (a + 8 < f.end) f.entries.insert(a + 8);
            }
        }
    }
    for (const auto& [from, t] : external) {
        Function* g = model.functionContaining(t);
        if (!g) {
            model.warnings.push_back({from, "desvio/chamada para " + anyps2::hex(t) +
                                                " fora do código conhecido"});
            continue;
        }
        if (t != g->start) g->entries.insert(t);
    }

    // ---- 4. Ponteiros para código em dados e constantes -------------------
    auto addEntry = [&](std::uint32_t t) {
        if (!inCode(t)) return;
        if (Function* g = model.functionContaining(t)) {
            if (t != g->start) g->entries.insert(t);
        }
    };
    if (options.scanDataPointers) {
        for (const auto& s : elf.sections()) {
            if (!s.isAlloc() || s.isExec() || s.type != elf::SHT_PROGBITS) continue;
            const auto data = elf.sectionData(s);
            for (std::size_t off = 0; off + 4 <= data.size(); off += 4) {
                const std::uint32_t v = static_cast<std::uint32_t>(data[off]) |
                                        (static_cast<std::uint32_t>(data[off + 1]) << 8) |
                                        (static_cast<std::uint32_t>(data[off + 2]) << 16) |
                                        (static_cast<std::uint32_t>(data[off + 3]) << 24);
                addEntry(v);
            }
        }
    }
    if (options.scanCodeConstants) {
        for (const auto& f : model.functions) {
            for (std::uint32_t a = f.start; a < f.end; a += 4) {
                const Instruction lui = r5900::decode(word(a), a);
                if (lui.op != Op::LUI || lui.rt() == 0) continue;
                const std::uint32_t hi = std::uint32_t{lui.imm16()} << 16;
                // Até 64 instruções depois do lui: compiladores separam o par (ex.:
                // lui no começo da função e addiu 11 instruções depois, no GT4).
                // Um par espúrio só acrescenta um ponto de entrada.
                for (std::uint32_t b = a + 4; b < f.end && b <= a + 256; b += 4) {
                    const Instruction lo = r5900::decode(word(b), b);
                    if ((lo.op == Op::ADDIU || lo.op == Op::ORI) && lo.rs() == lui.rt()) {
                        const std::uint32_t v = lo.op == Op::ADDIU
                                                    ? hi + static_cast<std::uint32_t>(lo.simm16())
                                                    : hi | lo.imm16();
                        addEntry(v);
                    }
                    if (lo.op == Op::LUI && lo.rt() == lui.rt()) break;
                }
            }
        }
    }

    // Entradas também são rótulos (destino do switch de entrada).
    for (auto& f : model.functions) {
        for (auto e : f.entries) f.labels.insert(e);
    }
    return model;
}

}  // namespace anyps2::analysis
