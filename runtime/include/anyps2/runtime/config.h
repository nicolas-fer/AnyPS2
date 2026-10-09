#pragma once

#include <functional>
#include <string>

#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

// Nome do arquivo de configuração padrão, ao lado do executável.
inline constexpr const char* kDefaultConfigFile = "anyps2.ini";
// Caminho do anyps2.ini ao lado do executável (não do diretório atual).
std::string defaultConfigPath();

// Nomes do SDL aceitos no arquivo. Com SDL (ANYPS2_SDL_NAMES) consulta o SDL;
// sem SDL não há como conferir e aceita qualquer nome (não há janela).
bool knownSdlKey(const std::string& name);
bool knownSdlButton(const std::string& name);
bool knownSdlAxis(const std::string& name);

// Opções do host, em três camadas (a última vence):
//   1. padrões (RuntimeOptions{});
//   2. arquivo INI: ANYPS2_CONFIG, ou anyps2.ini ao lado do executável;
//   3. variáveis de ambiente (ANYPS2_*).
// Com ANYPS2_CONFIG apontando para um arquivo que não existe, é erro; sem ela,
// a falta de anyps2.ini é normal. env é a fonte das variáveis (std::getenv no
// programa; os testes passam um mapa). defaultPath permite trocar anyps2.ini.
RuntimeOptions resolveOptions(const std::function<const char*(const char*)>& env, const std::string& defaultPath);

// Aplica o texto de um arquivo de configuração sobre o. source identifica o
// arquivo nas mensagens de erro ("anyps2.ini:3: ..."). Lança anyps2::Error na
// primeira linha inválida.
void applyConfigText(RuntimeOptions& o, const std::string& text, const std::string& source);

// Texto do anyps2.ini para estas opções: todas as seções e todas as ligações (as
// vazias como "chave =", para que desligar uma tecla padrão fique gravado).
// Ler o texto de volta (applyConfigText) dá as mesmas opções.
std::string configText(const RuntimeOptions& o);
// Grava configText(o) em path. Lança anyps2::Error se não conseguir.
void saveConfigFile(const RuntimeOptions& o, const std::string& path);

}  // namespace anyps2::rt
