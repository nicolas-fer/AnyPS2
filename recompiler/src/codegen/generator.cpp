#include "anyps2/codegen/generator.h"

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>

#include "anyps2/codegen/vu_generator.h"
#include "anyps2/common/bytes.h"
#include "anyps2/common/error.h"
#include "anyps2/r5900/decoder.h"
#include "anyps2/r5900/disassembler.h"

namespace anyps2::codegen {

using analysis::Function;
using analysis::ProgramModel;
using r5900::Format;
using r5900::Instruction;
using r5900::Op;

namespace {

std::string hex32(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08Xu", v);
    return buf;
}

std::string label(std::uint32_t a) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "L_%08X", a);
    return buf;
}

std::string cString(const std::string& s) {
    std::string out = "\"";
    for (char ch : s) {
        switch (ch) {
            case '\t': out += "\\t"; break;
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '?': out += "\\?"; break;  // evita trigraphs
            default: out += ch;
        }
    }
    return out + "\"";
}

std::string disasmComment(const Instruction& insn) {
    std::string text = r5900::disassemble(insn).str(' ');
    char head[24];
    std::snprintf(head, sizeof(head), "%08X: ", insn.address);
    return std::string("// ") + head + text;
}

bool isControl(const Instruction& insn) {
    return insn.hasDelaySlot() || insn.op == Op::SYSCALL || insn.op == Op::ERET;
}

bool isVuMacro(Format f) {
    switch (f) {
        case Format::FMT_VU_FD_FS_FT:
        case Format::FMT_VU_FD_FS_FTBC:
        case Format::FMT_VU_FD_FS_Q:
        case Format::FMT_VU_FD_FS_I:
        case Format::FMT_VU_ACC_FS_FT:
        case Format::FMT_VU_ACC_FS_FTBC:
        case Format::FMT_VU_ACC_FS_Q:
        case Format::FMT_VU_ACC_FS_I:
        case Format::FMT_VU_FT_FS:
        case Format::FMT_VU_CLIP:
        case Format::FMT_VU_ID_IS_IT:
        case Format::FMT_VU_IT_IS_IMM5:
        case Format::FMT_VU_CALLMS:
        case Format::FMT_VU_CALLMSR:
        case Format::FMT_VU_LQI:
        case Format::FMT_VU_SQI:
        case Format::FMT_VU_LQD:
        case Format::FMT_VU_SQD:
        case Format::FMT_VU_DIV:
        case Format::FMT_VU_SQRT:
        case Format::FMT_VU_MTIR:
        case Format::FMT_VU_MFIR:
        case Format::FMT_VU_ILWR:
        case Format::FMT_VU_RNEXT:
        case Format::FMT_VU_RINIT:
            return true;
        default:
            return false;
    }
}

class FunctionEmitter {
public:
    FunctionEmitter(const elf::ElfFile& elf, const ProgramModel& model, const Function& f,
                    GenerationReport& report)
        : elf_(elf), model_(model), f_(f), report_(report), labels_(f.labels) {
        // Se o delay slot de um desvio também é alvo, a cópia própria dele
        // termina com "goto" para a instrução seguinte, que precisa de rótulo.
        for (std::uint32_t a = f_.start; a < f_.end; a += 4) {
            const Instruction insn = decodeAt(a);
            if (insn.valid() && insn.hasDelaySlot() && labels_.count(a + 4)) labels_.insert(a + 8);
        }
    }

    std::string emit() {
        out_ << "// " << f_.name << " [" << hex32(f_.start) << ", " << hex32(f_.end) << ")\n";
        out_ << "void " << functionSymbol(f_.start) << "(Context* c) {\n";
        emitEntrySwitch();
        for (std::uint32_t a = f_.start; a < f_.end; a += 4) {
            if (labels_.count(a)) out_ << label(a) << ":;\n";
            const Instruction insn = decodeAt(a);
            ++report_.instructions;
            if (insn.valid() && isControl(insn) && insn.op != Op::SYSCALL && insn.op != Op::ERET) {
                emitControl(insn);
                // O delay slot já foi emitido dentro do bloco do desvio. Se ele
                // também é alvo de desvio, emite uma cópia própria.
                const std::uint32_t slot = a + 4;
                if (slot < f_.end && labels_.count(slot)) {
                    out_ << "    goto " << label(slot + 4) << ";\n";
                    out_ << label(slot) << ":;\n";
                    const Instruction s = decodeAt(slot);
                    out_ << "    " << disasmComment(s) << "\n";
                    out_ << "    " << simple(s) << "\n";
                }
                if (slot < f_.end) {
                    a += 4;
                    ++report_.instructions;
                }
                continue;
            }
            out_ << "    " << disasmComment(insn) << "\n";
            if (insn.valid() && insn.op == Op::ERET) {
                emitEret();
                continue;
            }
            out_ << "    " << simple(insn) << "\n";
        }
        // Execução que "cai" do fim da função continua no endereço seguinte.
        if (labels_.count(f_.end)) out_ << label(f_.end) << ":;\n";
        out_ << "    c->pc = " << hex32(f_.end) << ";\n";
        out_ << "    " << staticCall(f_.end) << "\n";
        out_ << "    return;\n";
        out_ << "}\n\n";

        return out_.str();
    }

private:
    Instruction decodeAt(std::uint32_t a) const {
        const auto w = elf_.tryRead32(a);
        return r5900::decode(w ? *w : 0xFFFFFFFFu, a);
    }

    void emitEntrySwitch() {
        out_ << "    if (c->pc != " << hex32(f_.start) << ") [[unlikely]] {\n";
        if (!f_.entries.empty()) {
            out_ << "        switch (c->pc) {\n";
            for (std::uint32_t e : f_.entries) {
                out_ << "            case " << hex32(e) << ": goto " << label(e) << ";\n";
            }
            out_ << "            default: break;\n";
            out_ << "        }\n";
        }
        out_ << "        badEntry(c, " << hex32(f_.start) << ");\n";
        out_ << "    }\n";
    }

    // Chamada estática para a função que contém `t` (c->pc já deve valer t).
    std::string staticCall(std::uint32_t t) const {
        const Function* g = model_.functionContaining(t);
        if (g && (t == g->start || g->entries.count(t))) return functionSymbol(g->start) + "(c);";
        return "c->rt->call(c);";
    }

    // Desvio sem link para t: goto local ou "tail call" e retorno.
    // Desvios para trás fecham laços: descontam o orçamento de instruções e,
    // quando ele acaba, passam por um safepoint (relógio, timers, VBlank,
    // interrupções e troca de thread).
    std::string jumpTo(std::uint32_t t, std::uint32_t from) const {
        if (f_.contains(t) && labels_.count(t)) {
            if (t > from) return "goto " + label(t) + ";";
            const std::uint32_t cost = (from - t) / 4 + 2;
            return "{ if ((c->budget -= " + std::to_string(cost) + ") < 0) [[unlikely]] c->rt->safepoint(c, " +
                   hex32(from) + "); goto " + label(t) + "; }";
        }
        return "{ c->pc = " + hex32(t) + "; " + staticCall(t) + " return; }";
    }

    // Chamada com retorno esperado em ret.
    std::string callTo(std::uint32_t t, std::uint32_t ret) const {
        return "c->pc = " + hex32(t) + "; " + staticCall(t) + " if (c->pc != " + hex32(ret) +
               ") [[unlikely]] return;";
    }

    std::string link(unsigned rd, std::uint32_t ret) const {
        if (rd == 0) return "";
        return "c->r[" + std::to_string(rd) + "].sd[0] = static_cast<std::int32_t>(" + hex32(ret) + ");";
    }

    std::string delaySlot(const Instruction& branch) {
        const Instruction s = decodeAt(branch.address + 4);
        std::string code = "        " + disasmComment(s) + "\n        ";
        if (s.valid() && isControl(s)) {
            code += unsupportedCall(s, "desvio dentro de delay slot");
        } else {
            code += simple(s);
        }
        return code + "\n";
    }

    std::string condition(const Instruction& i) const {
        const std::string rs = "c->r[" + std::to_string(i.rs()) + "]";
        const std::string rt = "c->r[" + std::to_string(i.rt()) + "]";
        switch (i.op) {
            case Op::BEQ: case Op::BEQL: return rs + ".ud[0] == " + rt + ".ud[0]";
            case Op::BNE: case Op::BNEL: return rs + ".ud[0] != " + rt + ".ud[0]";
            case Op::BLEZ: case Op::BLEZL: return rs + ".sd[0] <= 0";
            case Op::BGTZ: case Op::BGTZL: return rs + ".sd[0] > 0";
            case Op::BLTZ: case Op::BLTZL: case Op::BLTZAL: case Op::BLTZALL: return rs + ".sd[0] < 0";
            case Op::BGEZ: case Op::BGEZL: case Op::BGEZAL: case Op::BGEZALL: return rs + ".sd[0] >= 0";
            case Op::BC1F: case Op::BC1FL: return "!fpuCondition(c)";
            case Op::BC1T: case Op::BC1TL: return "fpuCondition(c)";
            case Op::BC0F: case Op::BC0FL: return "!cop0Condition(c)";
            case Op::BC0T: case Op::BC0TL: return "cop0Condition(c)";
            case Op::BC2F: case Op::BC2FL: return "!cop2Condition(c)";
            case Op::BC2T: case Op::BC2TL: return "cop2Condition(c)";
            default: return "";
        }
    }

    void emitControl(const Instruction& i) {
        const std::uint32_t ret = i.address + 8;
        out_ << "    " << disasmComment(i) << "\n";
        if (i.isBranch()) {
            const std::string cond = condition(i);
            if (cond.empty()) {
                out_ << "    " << unsupportedCall(i, "condição de desvio não tratada pelo gerador") << "\n";
                return;
            }
            const std::uint32_t t = i.branchTarget();
            std::string target;
            if (i.isLink()) {
                target = (f_.contains(t) && t != f_.start) ? jumpTo(t, i.address) : callTo(t, ret);
            } else {
                target = jumpTo(t, i.address);
            }
            out_ << "    {\n";
            out_ << "        const bool taken = " << cond << ";\n";
            if (i.isLink()) out_ << "        " << link(31, ret) << "\n";
            if (i.isLikely()) {
                out_ << "        if (taken) {\n" << indent(delaySlot(i)) << "            " << target << "\n        }\n";
            } else {
                out_ << delaySlot(i);
                out_ << "        if (taken) " << target << "\n";
            }
            out_ << "    }\n";
            return;
        }
        switch (i.op) {
            case Op::J:
                out_ << "    {\n" << delaySlot(i) << "        " << jumpTo(i.jumpTarget(), i.address) << "\n    }\n";
                return;
            case Op::JAL:
                out_ << "    {\n        " << link(31, ret) << "\n" << delaySlot(i);
                out_ << "        " << callTo(i.jumpTarget(), ret) << "\n    }\n";
                return;
            case Op::JR: {
                out_ << "    {\n        const std::uint32_t target = c->r[" << i.rs() << "].uw[0];\n";
                out_ << delaySlot(i);
                out_ << "        c->pc = target;\n";
                if (i.rs() != 31) emitLocalSwitch();
                out_ << "        return;\n    }\n";
                return;
            }
            case Op::JALR: {
                out_ << "    {\n        const std::uint32_t target = c->r[" << i.rs() << "].uw[0];\n";
                const std::string l = link(i.rd(), ret);
                if (!l.empty()) out_ << "        " << l << "\n";
                out_ << delaySlot(i);
                out_ << "        c->pc = target;\n        c->rt->call(c);\n";
                out_ << "        if (c->pc != " << hex32(ret) << ") [[unlikely]] return;\n    }\n";
                return;
            }
            default:
                out_ << "    " << unsupportedCall(i, "controle de fluxo não tratado pelo gerador") << "\n";
        }
    }

    // ERET: salto indireto para EPC/ErrorEPC (sem delay slot). Destino = ra é
    // um retorno, como "jr ra"; senão rótulo local ou despacho dinâmico.
    void emitEret() {
        out_ << "    {\n        const std::uint32_t target = ERET(c);\n";
        out_ << "        c->pc = target;\n";
        out_ << "        if (target != c->r[31].uw[0]) {\n";
        std::ostringstream body;
        std::swap(out_, body);
        emitLocalSwitch();
        std::swap(out_, body);
        out_ << indent(body.str()) << "        }\n        return;\n    }\n";
    }

    // jr para registrador: se o alvo é um rótulo desta função (jump table),
    // desvia localmente; senão é um tail call.
    void emitLocalSwitch() {
        if (!f_.labels.empty()) {
            out_ << "        switch (target) {\n";
            for (std::uint32_t l : f_.labels) {
                out_ << "            case " << hex32(l) << ": goto " << label(l) << ";\n";
            }
            out_ << "            default: break;\n        }\n";
        }
        out_ << "        c->rt->call(c);\n";
    }

    static std::string indent(const std::string& s) {
        std::string out;
        std::istringstream in(s);
        std::string line;
        while (std::getline(in, line)) out += "    " + line + "\n";
        return out;
    }

    // Macroinstrução do VU0: os bits 0–24 têm o mesmo layout do microcódigo;
    // as instruções "upper" (FMAC) vão para a palavra upper, as "lower"
    // (inteiros, FDIV, load/store do VU...) para o lower com opcode 0x40.
    std::string vuMacro(const Instruction& i) {
        const std::string pc = hex32(i.address);
        if (i.op == Op::VCALLMS) return "VCALLMS(c, " + hex32((i.raw >> 6) & 0x7FFF) + ", " + pc + ");";
        if (i.op == Op::VCALLMSR) return "VCALLMSR(c, " + pc + ");";
        const std::uint32_t f = i.raw & 0x3F;
        bool lowerType;
        if (f < 0x3C) {
            lowerType = f >= 0x30;
        } else {
            const std::uint32_t idx = (((i.raw >> 6) & 0x1F) << 2) | (f & 3);
            lowerType = idx >= 0x30;
        }
        const std::uint32_t body = i.raw & 0x01FFFFFFu;
        if (lowerType) return "vu0Macro(c, " + hex32((0x40u << 25) | body) + ", 0x000002FFu, " + pc + ");";
        return "vu0Macro(c, 0x8000033Cu, " + hex32(body) + ", " + pc + ");";
    }

    std::string unsupportedCall(const Instruction& i, const std::string& why) {
        const std::string text = r5900::disassemble(i).str(' ');
        report_.unsupported[std::string(i.valid() ? i.info().mnemonic : ".word")]++;
        return "unsupported(c, " + hex32(i.address) + ", " + cString(text + " — " + why) + ");";
    }

    std::string simple(const Instruction& i) {
        if (!i.valid()) {
            ++report_.invalidWords;
            return "unsupported(c, " + hex32(i.address) + ", " +
                   cString(r5900::disassemble(i).str(' ') + " — palavra que não é instrução do R5900") + ");";
        }
        const auto& info = i.info();
        const std::string name(info.name);
        const std::string pc = hex32(i.address);
        auto n = [](std::uint32_t v) { return std::to_string(v); };
        auto args = [&](std::initializer_list<std::string> list) {
            std::string s = name + "(c";
            for (const auto& a : list) s += ", " + a;
            return s + ");";
        };
        const bool needsPc = i.has(r5900::Flag::Overflow);
        if (isVuMacro(info.format)) return vuMacro(i);
        switch (info.format) {
            case Format::FMT_RD_RS_RT:
            case Format::FMT_RDOPT_RS_RT:
                return needsPc ? args({n(i.rd()), n(i.rs()), n(i.rt()), pc}) : args({n(i.rd()), n(i.rs()), n(i.rt())});
            case Format::FMT_RD_RT_RS: return args({n(i.rd()), n(i.rt()), n(i.rs())});
            case Format::FMT_RD_RT_SA: return args({n(i.rd()), n(i.rt()), n(i.sa())});
            case Format::FMT_RD_RT: return args({n(i.rd()), n(i.rt())});
            case Format::FMT_RD_RS: return args({n(i.rd()), n(i.rs())});
            case Format::FMT_RD: return args({n(i.rd())});
            case Format::FMT_RS: return args({n(i.rs())});
            case Format::FMT_RS_RT:
            case Format::FMT_ZERO_RS_RT: return args({n(i.rs()), n(i.rt())});
            case Format::FMT_RT_RS_SIMM:
                return needsPc ? args({n(i.rt()), n(i.rs()), std::to_string(i.simm16()), pc})
                               : args({n(i.rt()), n(i.rs()), std::to_string(i.simm16())});
            case Format::FMT_RT_RS_UIMM: return args({n(i.rt()), n(i.rs()), hex32(i.imm16())});
            case Format::FMT_RT_UIMM: return args({n(i.rt()), hex32(i.imm16())});
            case Format::FMT_MEM_GPR:
            case Format::FMT_MEM_FPR:
            case Format::FMT_MEM_VF: return args({n(i.rt()), n(i.rs()), std::to_string(i.simm16()), pc});
            case Format::FMT_CACHE: return "/* cache/pref: sem efeito no código recompilado */";
            case Format::FMT_SYNC: return "SYNC(c);";
            case Format::FMT_TRAP: return args({n(i.rs()), n(i.rt()), pc});
            case Format::FMT_RS_SIMM:
                if (i.has(r5900::Flag::Trap)) return args({n(i.rs()), std::to_string(i.simm16()), pc});
                return args({n(i.rs()), std::to_string(i.simm16())});
            case Format::FMT_RT_COP0: return args({n(i.rt()), n(i.rd()), pc});
            case Format::FMT_RT: return unsupportedCall(i, "registradores de debug do COP0");
            case Format::FMT_RT_PERF: return args({n(i.rt()), n((i.raw >> 1) & 31)});
            case Format::FMT_RT_FS:
            case Format::FMT_RT_FCR: return args({n(i.rt()), n(i.fs())});
            case Format::FMT_RT_VF: return args({n(i.rt()), n(i.rd())});
            case Format::FMT_RT_VI:
                if (i.op == Op::CTC2) return args({n(i.rt()), n(i.rd()), pc});
                return args({n(i.rt()), n(i.rd())});
            case Format::FMT_FD_FS_FT: return args({n(i.fd()), n(i.fs()), n(i.ft())});
            case Format::FMT_FD_FS: return args({n(i.fd()), n(i.fs())});
            case Format::FMT_FD_FT: return args({n(i.fd()), n(i.ft())});
            case Format::FMT_FS_FT: return args({n(i.fs()), n(i.ft())});
            case Format::FMT_BREAK: return "BREAK(c, " + pc + ");";
            case Format::FMT_SYSCALL: return "c->rt->syscall(c, " + pc + ");";
            case Format::FMT_NONE:
                switch (i.op) {
                    case Op::EI: return "EI(c);";
                    case Op::DI: return "DI(c);";
                    case Op::VNOP: return "/* vnop */";
                    case Op::VWAITQ: return "/* vwaitq: o resultado de Q já está disponível */";
                    default: return unsupportedCall(i, "operação de TLB do kernel (HLE)");
                }
            default:
                return unsupportedCall(i, "formato não tratado pelo gerador");
        }
    }

    const elf::ElfFile& elf_;
    const ProgramModel& model_;
    const Function& f_;
    GenerationReport& report_;
    std::set<std::uint32_t> labels_;
    std::ostringstream out_;
};

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw anyps2::Error("não foi possível criar " + path.string());
    out << content;
    if (!out) throw anyps2::Error("falha ao escrever " + path.string());
}

std::string cmakePath(const std::filesystem::path& p) {
    return p.generic_string();
}

}  // namespace

std::string functionSymbol(std::uint32_t start) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "fn_%08X", start);
    return buf;
}

std::string generateFunction(const elf::ElfFile& elf, const ProgramModel& model,
                             const Function& function, GenerationReport& report) {
    FunctionEmitter e(elf, model, function, report);
    return e.emit();
}

void writeImage(const elf::ElfFile& elf, const std::filesystem::path& path) {
    std::vector<std::uint8_t> out;
    auto put32 = [&out](std::uint32_t v) {
        const std::size_t at = out.size();
        out.resize(at + 4);
        anyps2::writeLE32(out, at, v);
    };
    const char magic[8] = {'A', 'P', '2', 'I', 'M', 'G', 0, 0};
    out.insert(out.end(), magic, magic + 8);
    put32(1);
    put32(elf.entry());
    std::uint32_t count = 0;
    for (const auto& s : elf.segments()) count += (s.isLoad() && s.memsz > 0) ? 1u : 0u;
    put32(count);
    for (const auto& s : elf.segments()) {
        if (!s.isLoad() || s.memsz == 0) continue;
        put32(s.vaddr);
        put32(s.filesz);
        put32(s.memsz);
        const auto data = elf.segmentData(s);
        out.insert(out.end(), data.begin(), data.end());
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) throw anyps2::Error("não foi possível criar " + path.string());
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
}

GenerationReport generateProject(const elf::ElfFile& elf, const ProgramModel& model,
                                 const GeneratorOptions& options) {
    namespace fs = std::filesystem;
    GenerationReport report;
    report.warnings = model.warnings;
    const fs::path src = options.outputDir / "src";
    fs::create_directories(src);

    const std::string name = options.projectName;
    const std::string imageName = name + ".image";
    writeImage(elf, options.outputDir / imageName);

    // Declarações.
    {
        std::ostringstream h;
        h << "// Gerado pelo AnyPS2 — não edite.\n#pragma once\n\n"
             "#include \"anyps2/runtime/context.h\"\n\n";
        for (const auto& f : model.functions) {
            h << "void " << functionSymbol(f.start) << "(::anyps2::rt::Context* c);\n";
        }
        writeFile(src / "functions.h", h.str());
    }

    // Corpos das funções, divididos em arquivos.
    std::vector<std::string> files;
    std::ostringstream cur;
    std::size_t curInsns = 0;
    auto startFile = [&]() {
        cur.str("");
        cur.clear();
        cur << "// Gerado pelo AnyPS2 a partir de " << elf.name() << " — não edite.\n"
            << "#include \"anyps2/runtime/generated.h\"\n#include \"functions.h\"\n\n"
            << "using namespace ::anyps2::rt;\nusing namespace ::anyps2::rt::ops;\n"
            << "using namespace ::anyps2::rt::gen;\n\n";
        curInsns = 0;
    };
    auto flushFile = [&]() {
        char fname[32];
        std::snprintf(fname, sizeof(fname), "functions_%03zu.cpp", files.size());
        writeFile(src / fname, cur.str());
        files.push_back(fname);
    };
    startFile();
    for (const auto& f : model.functions) {
        const std::size_t before = report.instructions;
        cur << generateFunction(elf, model, f, report);
        curInsns += report.instructions - before;
        ++report.functions;
        if (curInsns >= options.instructionsPerFile) {
            flushFile();
            startFile();
        }
    }
    if (curInsns > 0 || files.empty()) flushFile();

    // Microcódigo dos VUs.
    std::vector<vu::MicroBlob> blobs;
    if (options.recompileVu) {
        blobs = findElfMicrocode(elf);
        for (const auto& dump : options.vuDumps) vu::appendUnique(blobs, findDumpMicrocode(dump));
    }
    for (const auto& b : blobs) report.vuPairs += b.words.size() / 2;
    report.vuBlocks = blobs.size();
    std::vector<std::string> vuFiles;
    for (auto& vf : generateVuSources(blobs, "kVuPrograms")) {
        writeFile(src / vf.name, vf.text);
        vuFiles.push_back(vf.name);
    }

    // Tabela de funções + main.
    {
        std::ostringstream p;
        p << "// Gerado pelo AnyPS2 — não edite.\n"
             "#include \"anyps2/runtime/runtime.h\"\n#include \"functions.h\"\n\n"
             "extern const ::anyps2::rt::VuProgramEntry kVuPrograms[];\n"
             "extern const std::size_t kVuProgramsCount;\n\n"
             "namespace {\n"
             "const ::anyps2::rt::FunctionEntry kFunctions[] = {\n";
        for (const auto& f : model.functions) {
            p << "    {" << hex32(f.start) << ", " << hex32(f.end) << ", &" << functionSymbol(f.start)
              << ", " << cString(f.name) << "},\n";
        }
        p << "};\n"
             "const ::anyps2::rt::ProgramInfo kProgram = {\n"
          << "    " << cString(fs::path(elf.name()).filename().string()) << ",\n"
          << "    " << hex32(model.entry) << ",\n"
          << "    kFunctions,\n    sizeof(kFunctions) / sizeof(kFunctions[0]),\n"
          << "    " << cString(imageName) << ",\n"
          << "    kVuPrograms,\n    kVuProgramsCount,\n};\n}  // namespace\n\n"
          << "int main(int argc, char** argv) {\n"
             "    return ::anyps2::rt::runProgram(kProgram, argc, argv);\n}\n";
        writeFile(src / "program.cpp", p.str());
    }

    // CMakeLists.txt
    {
        std::ostringstream m;
        m << "# Projeto gerado pelo AnyPS2 a partir de " << fs::path(elf.name()).filename().string()
          << ".\n# Compile com: cmake -S . -B build && cmake --build build\n"
             "cmake_minimum_required(VERSION 3.20)\n"
          << "project(" << name << " LANGUAGES CXX)\n\n"
          << "set(ANYPS2_ROOT \"" << cmakePath(fs::absolute(options.anyps2Root))
          << "\" CACHE PATH \"Raiz do repositório AnyPS2\")\n"
             "include(${ANYPS2_ROOT}/cmake/AnyPS2Runtime.cmake)\n\n"
          << "add_executable(" << name << "\n    src/program.cpp\n";
        for (const auto& f : files) m << "    src/" << f << "\n";
        for (const auto& f : vuFiles) m << "    src/" << f << "\n";
        m << ")\n"
          << "anyps2_generated_target(" << name << " ${CMAKE_CURRENT_SOURCE_DIR}/" << imageName << ")\n";
        std::vector<std::string> vuBlocksFiles;
        for (const auto& f : vuFiles) {
            if (f != "vu_programs.cpp") vuBlocksFiles.push_back(f);
        }
        if (!vuBlocksFiles.empty()) {
            // Medido: -O1 compila o microcódigo gerado ~2x mais rápido que
            // -O2/-O3 com o mesmo desempenho (o custo está no núcleo do VU,
            // não no código de cada par).
            m << "set_source_files_properties(";
            for (const auto& f : vuBlocksFiles) m << "\n    src/" << f;
            m << "\n    PROPERTIES COMPILE_OPTIONS \"$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O1>\")\n";
        }
        writeFile(options.outputDir / "CMakeLists.txt", m.str());
    }
    report.sourceFiles = files.size() + vuFiles.size() + 1;
    return report;
}

}  // namespace anyps2::codegen
