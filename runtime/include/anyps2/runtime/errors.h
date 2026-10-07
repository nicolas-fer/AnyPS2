#pragma once

#include <cstdint>
#include <string>

#include "anyps2/common/error.h"

namespace anyps2::rt {

// Falha durante a execução do código do guest. Sempre traz o PC da
// instrução (ou do ponto de chamada) que provocou o erro.
class GuestError : public anyps2::Error {
public:
    GuestError(const std::string& message, std::uint32_t pc)
        : anyps2::Error(message + " (PC " + anyps2::hex(pc) + ")"), pc_(pc) {}
    std::uint32_t pc() const { return pc_; }

private:
    std::uint32_t pc_;
};

// Algo que o hardware/kernel real faria, mas que ainda não implementamos.
class Unimplemented : public GuestError {
public:
    using GuestError::GuestError;
};

// O programa terminou (syscall Exit ou equivalente). Não é um erro.
struct ProgramExit {
    int code;
};

}  // namespace anyps2::rt
