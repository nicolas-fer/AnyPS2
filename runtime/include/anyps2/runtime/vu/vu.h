#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "anyps2/runtime/vu/vu_core.h"

namespace anyps2::rt {

class Runtime;
class VuTrace;

// Estado do laço de execução de um microprograma, compartilhado entre o
// código recompilado e o interpretador (um pode continuar onde o outro parou,
// inclusive no meio de um delay slot).
struct VuCursor {
    std::uint32_t pc = 0;              // próximo par (bytes)
    bool hasDelayed = false;           // desvio tomado no par anterior
    std::uint32_t delayedTarget = 0;
    bool ending = false;               // o par anterior tinha o bit E
    bool done = false;                 // o delay slot do bit E já executou
};

// Bloco de microcódigo recompilado em C++ (gerado por "anyps2 recomp").
//
// O bloco não depende do endereço em que foi carregado na micro memória
// (`base`): os desvios do VU são relativos e os alvos de JR/JALR são
// resolvidos em tempo de execução. Cada par confere, antes de executar, que
// a micro memória ainda contém exatamente a instrução compilada; se não
// contém (outro programa carregado por cima, upload parcial), o bloco
// devolve o controle e o interpretador continua dali.
using VuBlockFn = void (*)(class Vu& vu, std::uint32_t base, VuCursor& cur);

struct VuProgramEntry {
    std::uint32_t loadHint;        // endereço de carga conhecido (MPG) ou kNoLoadHint
    const std::uint32_t* words;    // microcódigo (pares lower, upper)
    std::uint32_t wordCount;
    VuBlockFn fn;
    const char* name;              // origem (para mensagens e estatísticas)
};
inline constexpr std::uint32_t kNoLoadHint = 0xFFFFFFFFu;

// Um Vector Unit em modo micro (VU0 via VCALLMS, VU1 via MSCAL/CMSAR1).
//
// A execução é síncrona: o microprograma roda inteiro quando é iniciado (o
// EE "espera" de graça). Os pares vêm do código recompilado quando há um
// bloco com a mesma instrução no pc (ver VuProgramEntry) e do interpretador
// nos demais casos. Dentro do microprograma o tempo é modelado em
// ciclos (1 por par + stalls) para reproduzir o que o código observa:
//  * registradores VF escritos ficam prontos 4 ciclos depois; ler antes
//    trava (stall) como no hardware;
//  * as flags MAC/status/clip de uma instrução só ficam visíveis para
//    FMAND/FSAND/FCAND... 4 ciclos depois;
//  * Q (DIV 7, SQRT 7, RSQRT 13 ciclos) e P (EFU, 11–54 ciclos) mantêm o
//    valor antigo até a operação terminar; WAITQ/WAITP e um novo DIV/EFU
//    esperam.
class Vu {
public:
    Vu(Runtime* rt, unsigned unit, vucore::Regs regs, std::uint8_t* data, std::uint32_t dataSize,
       std::uint8_t* micro, std::uint32_t microSize);

    unsigned unit() const { return unit_; }
    const vucore::Regs& regs() const { return regs_; }
    std::uint8_t* data() { return data_; }
    std::uint32_t dataSize() const { return dataSize_; }
    std::uint8_t* micro() { return micro_; }
    std::uint32_t microSize() const { return microSize_; }

    // Inicia o microprograma em pc (bytes) e executa até o bit E.
    void start(std::uint32_t pc, std::uint32_t eePc);
    enum class Mode {
        Auto,         // recompilado quando houver, senão interpretado
        Interpret,    // só o interpretador (ANYPS2_VU=interp)
        CompiledOnly  // erro se algum par não tiver versão recompilada (ANYPS2_VU=compiled)
    };
    void setMode(Mode m) { mode_ = m; }
    // ANYPS2_VU_DUMP: grava a micro memória dos programas interpretados em
    // dir/vu<unidade>_<hash>.bin (entrada para "anyps2 recomp --vu-dumps").
    void setDumpDir(std::string dir) { dumpDir_ = std::move(dir); }
    // ANYPS2_VU_TRACE: rastreador de dados (vu_trace.h). Nulo desliga; não
    // é dono do objeto.
    void setTrace(VuTrace* t) {
        trace_ = t;
        tracing_ = t != nullptr;
    }
    // Modo macro (só VU0): executa uma instrução COP2 vinda do EE e conclui o
    // pipeline (o EE vê o resultado, as flags e Q imediatamente).
    void macro(const vu::Instr& in, std::uint32_t eePc);
    // Só o interpretador (usado também como oráculo nos testes).
    void interpret(std::uint32_t pc, std::uint32_t eePc);
    // Blocos recompilados disponíveis (os mesmos para VU0 e VU1).
    void setPrograms(const VuProgramEntry* programs, std::size_t count);
    void setInterpretOnly(bool v) { mode_ = v ? Mode::Interpret : Mode::Auto; }
    // Execuções que usaram algum par recompilado / nenhum.
    std::uint64_t compiledRuns() const { return compiledRuns_; }
    std::uint64_t interpretedRuns() const { return interpretedRuns_; }
    std::uint64_t compiledPairs() const { return compiledPairs_; }
    std::uint64_t interpretedPairs() const { return interpretedPairs_; }
    void reset();

    // ---- Execução de um par (interpretador e código recompilado) -----------
    struct Flow {
        bool taken = false;          // desvio tomado (alvo em target)
        std::uint32_t target = 0;    // bytes
    };
    void execPair(const vu::Instr& in, std::uint32_t pc, Flow& flow);
    // Com as operações fixas em tempo de compilação (código recompilado).
    template <vu::U UOp, vu::L LOp>
    void execPairOp(const vu::Instr& in, std::uint32_t pc, Flow& flow);
    // Executa o par em pc e avança o cursor (delay slot, bit E). O código
    // recompilado passa as operações como parâmetros do template; COUNT_ =
    // decide pela instrução.
    template <vu::U UOp = vu::U::COUNT_, vu::L LOp = vu::L::COUNT_>
    void step(const vu::Instr& in, std::uint32_t pc, VuCursor& cur);
    // A micro memória contém este par em pc?
    bool pairIs(std::uint32_t pc, std::uint32_t lowerWord, std::uint32_t upperWord) const;
    std::uint32_t wrap(std::uint32_t pc) const { return pc & (microSize_ - 1); }
    // Fim do microprograma: conclui o pipeline (flags, Q, P pendentes).
    void finish();
    // Limite de pares por execução (laço infinito vira erro).
    void countPair(std::uint32_t pc);
    std::uint64_t cycle() const { return cycle_; }
    std::uint32_t startPc() const { return startPc_; }

private:
    struct PendingFlags {
        std::uint64_t ready;
        bool hasMac;
        std::uint16_t mac;
        bool hasClip;
        std::uint32_t clip;
    };
    struct PendingUnit {  // Q (FDIV) ou P (EFU)
        bool active = false;
        std::uint64_t ready = 0;
        std::uint32_t value = 0;
        std::uint32_t flags = 0;
    };

    struct Match {
        const VuProgramEntry* entry = nullptr;
        std::uint32_t base = 0;
    };
    Match findBlock(std::uint32_t pc) const;
    void run(std::uint32_t pc, std::uint32_t eePc, bool compiled);
    const vu::Instr& fetch(std::uint32_t pc);
    void dumpMicro();

    // Parte comum de todo par (não depende da operação), inline em vu_exec.h:
    // validação, flags/Q/P prontos e stalls antes; escrita do upper, flags
    // pendentes, LOI e ciclo depois.
    void pairBegin(const vu::Instr& in, std::uint32_t pc);
    void pairEnd(const vu::Instr& in, const vucore::UpperResult& ur);
    void pairRejected(const vu::Instr& in, std::uint32_t pc) const;  // caminho de erro, fora de linha

    void commitReady();
    void commitPending();
    static constexpr std::uint64_t kNoCommit = ~std::uint64_t{0};
    void commitQ();
    void commitP();
    void stallOn(unsigned vf);
    void stallUntil(std::uint64_t c) {
        if (c > cycle_) cycle_ = c;
    }
    Reg128& mem(std::uint32_t qwordIndex, std::uint32_t pc);
    Reg128& memVu0Cross(std::uint32_t index, std::uint32_t pc);  // VU0 → registradores do VU1
    void execLower(const vu::Instr& in, std::uint32_t pc, Flow& flow);
    template <vu::L Op>
    void execLowerOp(const vu::Instr& in, std::uint32_t pc, Flow& flow);
    void execEfu(const vu::Lower& l, std::uint32_t pc);
    void xgkick(std::uint32_t addr, std::uint32_t pc);
    [[noreturn]] void tooManyPairs(std::uint32_t pc) const;
    static constexpr std::uint64_t kMaxPairs = 200u * 1000 * 1000;
    std::uint32_t vifTop(bool itop, std::uint32_t pc);  // XTOP/XITOP
    [[noreturn]] void fail(const std::string& what, std::uint32_t pc) const;
    std::string where(std::uint32_t pc) const;
    std::string macroWhere() const;

    Runtime* rt_;
    unsigned unit_;
    vucore::Regs regs_;
    std::uint8_t* data_;
    std::uint32_t dataSize_;
    std::uint8_t* micro_;
    std::uint32_t microSize_;

    // Pipeline
    std::uint64_t cycle_ = 0;
    std::array<std::uint64_t, 32> vfReady_{};
    std::uint64_t accReady_ = 0;
    static constexpr unsigned kPending = 16;
    std::array<PendingFlags, kPending> pending_{};
    unsigned pendHead_ = 0, pendCount_ = 0;
    PendingUnit q_, p_;
    std::uint64_t nextCommit_ = kNoCommit;  // ≤ primeiro evento pendente

    // Execução
    std::uint32_t startPc_ = 0;
    std::uint32_t eePc_ = 0;
    std::uint64_t pairs_ = 0;
    bool inMacro_ = false;
    Mode mode_ = Mode::Auto;
    std::uint64_t compiledRuns_ = 0, interpretedRuns_ = 0;
    std::uint64_t compiledPairs_ = 0, interpretedPairs_ = 0;
    // Índice dos blocos recompilados pelo conteúdo de cada par:
    // par (upper << 32 | lower) → (bloco, deslocamento em bytes).
    struct BlockRef {
        const VuProgramEntry* entry;
        std::uint32_t offset;
    };
    std::unordered_multimap<std::uint64_t, BlockRef> index_;
    std::string dumpDir_;
    std::unordered_set<std::uint64_t> dumped_;
    VuTrace* trace_ = nullptr;   // rastreador (ANYPS2_VU_TRACE), não é dono
    bool tracing_ = false;       // trace_ != nullptr: o único teste nos caminhos quentes

    // Cache de decodificação do interpretador
    struct Cached {
        std::uint32_t lo = 0, up = 0;
        bool valid = false;
        vu::Instr in;
    };
    std::vector<Cached> cache_;
};

}  // namespace anyps2::rt
