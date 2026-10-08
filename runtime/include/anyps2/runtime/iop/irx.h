#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace anyps2::rt {

// Lê o cabeçalho .iopmod de um IRX: nome e versão do módulo.
struct IrxInfo {
    std::string name;
    std::uint16_t version = 0;
};
std::optional<IrxInfo> parseIrx(const std::uint8_t* data, std::size_t size);

// Tabela de módulos do IOP com implementação HLE (a mesma que o loadfile usa
// para decidir se um módulo carrega). Devolvem o nome da implementação
// ("padman", "cdvdman"...) ou nullptr se o módulo não tem HLE.
//   hleModuleForIrx: pelo nome gravado no IRX ("padman", "cdvd_driver");
//   hleModuleForRom: por um módulo da ROM do console ("rom0:XPADMAN",
//                    "rom0:PADMAN.IRX" ou só "XPADMAN").
const char* hleModuleForIrx(const std::string& irxName);
const char* hleModuleForRom(const std::string& romPath);
// "rom0:XPADMAN" / "rom0:PADMAN.IRX" → "XPADMAN"
std::string romModuleName(const std::string& romPath);

}  // namespace anyps2::rt
