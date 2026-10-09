#pragma once

#include <functional>
#include <string>

#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

// Arquivo de configuração padrão, lido do diretório atual se existir.
inline constexpr const char* kDefaultConfigFile = "anyps2.ini";

// Opções do host, em três camadas (a última vence):
//   1. padrões (RuntimeOptions{});
//   2. arquivo INI: ANYPS2_CONFIG, ou anyps2.ini se a variável não existir;
//   3. variáveis de ambiente (ANYPS2_*).
// Com ANYPS2_CONFIG apontando para um arquivo que não existe, é erro; sem ela,
// a falta de anyps2.ini é normal. env é a fonte das variáveis (std::getenv no
// programa; os testes passam um mapa). defaultPath permite trocar anyps2.ini.
RuntimeOptions resolveOptions(const std::function<const char*(const char*)>& env,
                              const std::string& defaultPath = kDefaultConfigFile);

// Aplica o texto de um arquivo de configuração sobre o. source identifica o
// arquivo nas mensagens de erro ("anyps2.ini:3: ..."). Lança anyps2::Error na
// primeira linha inválida.
void applyConfigText(RuntimeOptions& o, const std::string& text, const std::string& source);

}  // namespace anyps2::rt
