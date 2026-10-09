#include "anyps2/runtime/config.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <vector>

#include "anyps2/common/error.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace anyps2::rt {

namespace {

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Corta o comentário: "#" ou ";" no começo da linha, ou depois de espaço (um
// "#" colado num valor, como em C:\a#b, faz parte do valor).
std::string stripComment(const std::string& line) {
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if ((c == '#' || c == ';') && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
            return line.substr(0, i);
        }
    }
    return line;
}

// Inteiro decimal inteiro (sem lixo depois) dentro de [lo, hi].
bool parseInt(const std::string& text, int lo, int hi, int& out) {
    if (text.empty()) return false;
    std::size_t used = 0;
    long v = 0;
    try {
        v = std::stol(text, &used);
    } catch (const std::exception&) {
        return false;
    }
    if (used != text.size() || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

// "sdl", "none" ou "auto" (automático = "" em RuntimeOptions::video/audio).
bool parseMode(const std::string& value, std::string& out) {
    const std::string m = lower(value);
    if (m == "sdl" || m == "none") {
        out = m;
        return true;
    }
    if (m == "auto") {
        out.clear();
        return true;
    }
    return false;
}

bool isSection(const std::string& s) {
    static const char* const kSections[] = {"memcard", "disc",       "video",     "audio",
                                            "keyboard.1", "keyboard.2", "gamepad.1", "gamepad.2"};
    for (const char* name : kSections) {
        if (s == name) return true;
    }
    return false;
}

int analogIndex(const std::string& name) {
    for (int i = 0; i < 4; ++i) {
        if (name == kPadAnalogNames[i]) return i;
    }
    return -1;
}

std::string buttonList() {
    std::string list;
    for (int i = 0; i < 16; ++i) list += std::string(i ? ", " : "") + kPadButtonNames[i];
    return list;
}

// Aplica "chave = valor" dentro de uma seção. Devolve o motivo do erro, ou ""
// se deu certo.
std::string setValue(RuntimeOptions& o, const std::string& section, const std::string& key,
                     const std::string& value) {
    if (section == "memcard") {
        if (key != "dir") return "chave desconhecida '" + key + "' em [memcard] (use dir)";
        o.memcardDir = value;
    } else if (section == "disc") {
        if (key != "iso") return "chave desconhecida '" + key + "' em [disc] (use iso)";
        o.iso = value;
    } else if (section == "video") {
        if (key == "mode") {
            if (!parseMode(value, o.video)) {
                return "valor inválido para [video] mode: '" + value + "' (use sdl, none ou auto)";
            }
        } else if (key == "scale") {
            if (!parseInt(value, 1, 4, o.videoScale)) {
                return "valor inválido para [video] scale: '" + value + "' (use um número de 1 a 4)";
            }
        } else {
            return "chave desconhecida '" + key + "' em [video] (use mode ou scale)";
        }
    } else if (section == "audio") {
        if (key == "mode") {
            if (!parseMode(value, o.audio)) {
                return "valor inválido para [audio] mode: '" + value + "' (use sdl, none ou auto)";
            }
        } else if (key == "wav") {
            o.audioWav = value;
        } else {
            return "chave desconhecida '" + key + "' em [audio] (use mode ou wav)";
        }
    } else {
        // keyboard.N e gamepad.N: a porta é o número depois do ponto (1 ou 2).
        const bool keyboard = section.compare(0, 9, "keyboard.") == 0;
        const unsigned port = static_cast<unsigned>(section.back() - '1');
        const int button = padButtonIndex(key);
        const int analog = analogIndex(key);
        const std::string where = " em [" + section + "] " + key;
        if (button < 0 && analog < 0) {
            return "botão desconhecido '" + key + "' em [" + section + "] (use " + buttonList() +
                   ", ou lx, ly, rx, ry no controle)";
        }
        if (keyboard) {
            if (button < 0) {
                return "'" + key + "' é analógico e não tem tecla: ligue-o em [gamepad." +
                       std::to_string(port + 1) + "]";
            }
            if (!value.empty() && !knownSdlKey(value)) return "tecla desconhecida '" + value + "'" + where;
            o.pad.key[port][button] = value;
            return "";
        }
        // Controle: botão digital aceita eixo com sinal (+leftx, -leftx); analógico
        // só aceita eixo, sem sinal.
        int dir = 0;
        const std::string name = padAxisName(value, dir);
        if (!value.empty()) {
            if (button >= 0) {
                if (!knownSdlButton(name) && !knownSdlAxis(name)) {
                    return "botão do controle desconhecido '" + value + "'" + where;
                }
                if (dir != 0 && !knownSdlAxis(name)) {
                    return "sinal (+ ou -) só vale para eixos, não para '" + name + "'" + where;
                }
            } else {
                if (dir != 0) return "eixo analógico não aceita sinal (+ ou -): '" + value + "'" + where;
                if (!knownSdlAxis(name)) return "eixo do controle desconhecido '" + value + "'" + where;
            }
        }
        if (button >= 0) {
            o.pad.button[port][button] = value;
        } else {
            o.pad.analog[port][analog] = value;
        }
    }
    return "";
}

// Pasta do executável (o anyps2.ini padrão fica ao lado dele).
std::filesystem::path executableDir() {
#ifdef _WIN32
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return ".";
    return std::filesystem::path(std::wstring(buf.data(), n)).parent_path();
#else
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path(".") : exe.parent_path();
#endif
}

// Variáveis de ambiente: sobrepõem o arquivo.
void applyEnvironment(RuntimeOptions& o, const std::function<const char*(const char*)>& env) {
    if (const char* t = env("ANYPS2_TRACE")) {
        const std::string s(t);
        o.traceSyscalls = s.find("syscall") != std::string::npos || s == "all";
        o.traceCalls = s.find("call") != std::string::npos || s == "all";
        o.traceHardware = s.find("hw") != std::string::npos || s == "all";
        o.traceIop = s.find("iop") != std::string::npos || s == "all";
        o.traceGs = s.find("gs") != std::string::npos || s == "all";
        o.traceThreads = s.find("threads") != std::string::npos || s == "all";
    }
    if (const char* clock = env("ANYPS2_CLOCK")) o.virtualClock = std::string(clock) == "virtual";
    if (const char* p = env("ANYPS2_PROFILE")) o.profile = *p && std::string(p) != "0";
    if (const char* v = env("ANYPS2_VIDEO")) o.video = v;
    if (const char* v = env("ANYPS2_VIDEO_SCALE")) {
        if (!parseInt(v, 1, 4, o.videoScale)) {
            throw anyps2::Error(std::string("ANYPS2_VIDEO_SCALE='") + v + "' inválido (use 1 a 4)");
        }
    }
    if (const char* shot = env("ANYPS2_SCREENSHOT")) o.screenshot = shot;
    if (const char* n = env("ANYPS2_SCREENSHOT_EVERY")) o.screenshotEvery = std::strtoull(n, nullptr, 10);
    if (const char* v = env("ANYPS2_VU")) o.vuMode = v;
    if (const char* d = env("ANYPS2_VU_DUMP")) o.vuDumpDir = d;
    if (const char* n = env("ANYPS2_FRAMES")) o.frames = std::strtoull(n, nullptr, 10);
    if (const char* v = env("ANYPS2_ISO")) o.iso = v;
    if (const char* v = env("ANYPS2_PAD_SCRIPT")) o.padScript = v;
    if (const char* v = env("ANYPS2_AUDIO")) o.audio = v;
    if (const char* v = env("ANYPS2_AUDIO_WAV")) o.audioWav = v;
    if (const char* v = env("ANYPS2_MC_DIR")) o.memcardDir = v;
}

}  // namespace

std::string configText(const RuntimeOptions& o) {
    std::ostringstream out;
    out << "# anyps2.ini: gravado pelo menu de configuração (F1 na janela).\n"
        << "# Variáveis de ambiente, quando definidas, continuam valendo por cima.\n\n";
    out << "[memcard]\ndir = " << o.memcardDir << "\n\n";
    out << "[disc]\niso = " << o.iso << "\n\n";
    out << "[video]\nmode = " << (o.video.empty() ? "auto" : o.video) << "\nscale = " << o.videoScale << "\n\n";
    out << "[audio]\nmode = " << (o.audio.empty() ? "auto" : o.audio) << "\nwav = " << o.audioWav << "\n\n";
    for (unsigned port = 0; port < 2; ++port) {
        out << "[keyboard." << port + 1 << "]\n";
        for (int i = 0; i < 16; ++i) out << kPadButtonNames[i] << " = " << o.pad.key[port][i] << "\n";
        out << "\n";
    }
    for (unsigned port = 0; port < 2; ++port) {
        out << "[gamepad." << port + 1 << "]\n";
        for (int i = 0; i < 16; ++i) out << kPadButtonNames[i] << " = " << o.pad.button[port][i] << "\n";
        for (int a = 0; a < 4; ++a) out << kPadAnalogNames[a] << " = " << o.pad.analog[port][a] << "\n";
        out << "\n";
    }
    return out.str();
}

void saveConfigFile(const RuntimeOptions& o, const std::string& path) {
    std::ofstream f(path, std::ios::binary);
    if (!f) throw anyps2::Error("não foi possível gravar " + path);
    f << configText(o);
    f.close();
    if (!f) throw anyps2::Error("erro ao gravar " + path);
}

std::string defaultConfigPath() {
    return (executableDir() / kDefaultConfigFile).string();
}

void applyConfigText(RuntimeOptions& o, const std::string& text, const std::string& source) {
    std::string section;  // vazio até a primeira seção
    std::istringstream in(text);
    std::string raw;
    unsigned lineNo = 0;
    const auto fail = [&](const std::string& why) {
        throw anyps2::Error(source + ":" + std::to_string(lineNo) + ": " + why);
    };
    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = trim(stripComment(raw));
        if (line.empty()) continue;
        if (line.front() == '[') {
            if (line.back() != ']') fail("seção sem ']' no fim: '" + line + "'");
            section = lower(trim(line.substr(1, line.size() - 2)));
            if (!isSection(section)) {
                fail("seção desconhecida [" + section +
                     "] (use memcard, disc, video, audio, keyboard.1, keyboard.2, gamepad.1 ou gamepad.2)");
            }
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string::npos) fail("esperado 'chave = valor' ou [seção], achei: '" + line + "'");
        const std::string key = lower(trim(line.substr(0, eq)));
        const std::string value = trim(line.substr(eq + 1));
        if (section.empty()) fail("'" + key + "' está fora de qualquer seção (comece com [video], [disc] etc.)");
        if (key.empty()) fail("falta o nome da chave antes do '='");
        const std::string why = setValue(o, section, key, value);
        if (!why.empty()) fail(why);
    }
}

RuntimeOptions resolveOptions(const std::function<const char*(const char*)>& env, const std::string& defaultPath) {
    RuntimeOptions o;
    const char* explicitPath = env("ANYPS2_CONFIG");
    const bool given = explicitPath && *explicitPath;
    const std::string path = given ? explicitPath : defaultPath;
    o.configPath = path;
    std::ifstream f(path, std::ios::binary);
    if (f) {
        std::stringstream text;
        text << f.rdbuf();
        applyConfigText(o, text.str(), path);
    } else if (given) {
        throw anyps2::Error("ANYPS2_CONFIG: não foi possível ler " + path);
    }
    applyEnvironment(o, env);
    return o;
}

}  // namespace anyps2::rt
