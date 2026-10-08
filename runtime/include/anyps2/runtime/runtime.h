#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/context.h"
#include "anyps2/runtime/memory.h"

namespace anyps2::rt {

class Kernel;
class Hardware;
class Iop;
class Timing;
class Dmac;
class Gif;
class Vif;
struct VuMemory;
class Video;
class Input;
class Audio;
class Vu;
struct VuProgramEntry;
namespace gs {
class Gs;
}

using GuestFunction = void (*)(Context*);

// Uma função recompilada: cobre [start, end) no espaço do EE.
struct FunctionEntry {
    std::uint32_t start;
    std::uint32_t end;
    GuestFunction fn;
    const char* name;
};

// Descrição do programa recompilado, gerada pelo recompilador.
struct ProgramInfo {
    const char* name;            // nome do ELF de origem
    std::uint32_t entry;         // ponto de entrada
    const FunctionEntry* functions;  // ordenado por start
    std::size_t functionCount;
    const char* imageFile;       // arquivo com os segmentos (ao lado do executável)
    // Microprogramas dos VUs recompilados (opcional).
    const VuProgramEntry* vuPrograms = nullptr;
    std::size_t vuProgramCount = 0;
};

// Endereço mágico de retorno ao host: quando o runtime chama código do guest
// (handlers de interrupção, callbacks), ra recebe este valor; "jr ra" para
// ele devolve o controle ao C++.
inline constexpr std::uint32_t kHostReturn = 0xFFFFFFF0u;

struct RuntimeOptions {
    bool traceSyscalls = false;  // ANYPS2_TRACE contém "syscall"
    bool traceCalls = false;     // ANYPS2_TRACE contém "call"
    bool traceHardware = false;  // ANYPS2_TRACE contém "hw"
    bool traceIop = false;       // ANYPS2_TRACE contém "iop"
    bool traceGs = false;        // ANYPS2_TRACE contém "gs" (DMA, GIF, VIF, VU)
    // ANYPS2_VU: "interp" ignora os microprogramas recompilados; "compiled"
    // exige que todo par executado tenha versão recompilada (testes).
    std::string vuMode;
    std::string vuDumpDir;        // ANYPS2_VU_DUMP: grava microprogramas interpretados
    bool virtualClock = false;   // ANYPS2_CLOCK=virtual (determinístico)
    // ANYPS2_VIDEO: "sdl" (janela), "none" (sem janela) ou "" (automático:
    // janela se o runtime foi compilado com SDL e há display).
    std::string video;
    // ANYPS2_SCREENSHOT: grava a última imagem exibida (PNG) ao terminar.
    std::string screenshot;
    // ANYPS2_FRAMES=N: encerra o programa (código 0) no N-ésimo VBlank. Para
    // testar programas que desenham em laço infinito.
    std::uint64_t frames = 0;
    // ANYPS2_ISO: imagem do disco (dump próprio) lida pelo cdvdfsv/cdrom0:.
    std::string iso;
    // ANYPS2_PAD_SCRIPT: roteiro determinístico dos controles (ver input.h).
    std::string padScript;
    // ANYPS2_AUDIO: "sdl", "none" ou "" (automático). ANYPS2_AUDIO_WAV:
    // grava o som num WAV.
    std::string audio;
    std::string audioWav;
    static RuntimeOptions fromEnvironment();
};

class Runtime {
public:
    explicit Runtime(const ProgramInfo& program, RuntimeOptions options = RuntimeOptions::fromEnvironment());
    ~Runtime();
    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    // Carrega a imagem, prepara o contexto e executa até Exit. Retorna o
    // código de saída do programa.
    int run(const std::vector<std::string>& args, const std::string& imagePath);

    // ---- Usado pelo código gerado -----------------------------------------
    // Chama a função que contém c->pc (despacho indireto: JALR, JR para
    // registrador, tail calls).
    void call(Context* c);
    // Executa uma syscall (v1 = número).
    void syscall(Context* c, std::uint32_t pc);
    // Chamado pelo código gerado quando o orçamento de instruções acaba (e no
    // fim das syscalls): avança o relógio, dispara eventos, entrega
    // interrupções e pode trocar de thread.
    void safepoint(Context* c, std::uint32_t pc, std::int64_t extraCycles = 0);

    // ---- Usado pelo HLE -----------------------------------------------------
    const FunctionEntry* lookup(std::uint32_t address) const;
    // Executa uma função do guest a partir do host, preservando o contexto
    // atual. Retorna v0.
    std::uint64_t invokeGuest(std::uint32_t address, const std::vector<std::uint32_t>& args,
                              std::uint32_t pc);

    Context& context() { return *ctx_; }
    Memory& memory() { return *mem_; }
    Kernel& kernel() { return *kernel_; }
    Hardware& hardware() { return *hw_; }
    Iop& iop() { return *iop_; }
    Timing& timing() { return *timing_; }
    gs::Gs& gs() { return *gs_; }
    Gif& gif() { return *gif_; }
    Vif& vif0() { return *vif0_; }
    Vif& vif1() { return *vif1_; }
    Dmac& dmac() { return *dmac_; }
    VuMemory& vu() { return *vu_; }
    Vu& vu0() { return *vu0_; }
    Vu& vu1() { return *vu1_; }
    Input& input() { return *input_; }
    // Saída de som; nullptr fora de run().
    Audio* audio() { return audio_.get(); }
    // Início do VBlank: módulos periódicos do IOP (pad, áudio), apresenta o
    // quadro e processa eventos da janela.
    void onVblank(std::uint32_t pc);
    const RuntimeOptions& options() const { return options_; }
    const ProgramInfo& program() const { return program_; }

    // Converte endereço em "função+offset" para mensagens.
    std::string describe(std::uint32_t address) const;

    // Despacha funções a partir de c->pc até c->pc == stopPc.
    void runUntil(std::uint32_t stopPc);

private:
    void loadImage(const std::string& path);
    void saveScreenshot();

    ProgramInfo program_;
    RuntimeOptions options_;
    std::unique_ptr<Memory> mem_;
    std::unique_ptr<Context> ctx_;
    std::unique_ptr<Hardware> hw_;
    std::unique_ptr<Iop> iop_;
    std::unique_ptr<Timing> timing_;
    std::unique_ptr<Kernel> kernel_;
    std::unique_ptr<VuMemory> vu_;
    std::unique_ptr<gs::Gs> gs_;
    std::unique_ptr<Gif> gif_;
    std::unique_ptr<Vif> vif0_, vif1_;
    std::unique_ptr<Dmac> dmac_;
    struct Vu1Regs {
        Reg128 vf[32];
        std::uint32_t vi[32];
        Reg128 acc;
    };
    std::unique_ptr<Vu1Regs> vu1Regs_;
    std::unique_ptr<Vu> vu0_, vu1_;
    std::unique_ptr<Video> video_;
    std::unique_ptr<Input> input_;
    std::unique_ptr<Audio> audio_;
    std::uint64_t vblanks_ = 0;
};

// Ponto de entrada usado pelo main() gerado.
int runProgram(const ProgramInfo& program, int argc, char** argv);

}  // namespace anyps2::rt
