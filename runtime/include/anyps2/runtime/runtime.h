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
    const RuntimeOptions& options() const { return options_; }
    const ProgramInfo& program() const { return program_; }

    // Converte endereço em "função+offset" para mensagens.
    std::string describe(std::uint32_t address) const;

    // Despacha funções a partir de c->pc até c->pc == stopPc.
    void runUntil(std::uint32_t stopPc);

private:
    void loadImage(const std::string& path);

    ProgramInfo program_;
    RuntimeOptions options_;
    std::unique_ptr<Memory> mem_;
    std::unique_ptr<Context> ctx_;
    std::unique_ptr<Hardware> hw_;
    std::unique_ptr<Iop> iop_;
    std::unique_ptr<Kernel> kernel_;
};

// Ponto de entrada usado pelo main() gerado.
int runProgram(const ProgramInfo& program, int argc, char** argv);

}  // namespace anyps2::rt
