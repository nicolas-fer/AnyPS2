#include "anyps2/runtime/input.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "anyps2/common/error.h"

namespace anyps2::rt {

std::uint16_t parsePadButtons(const std::string& text) {
    if (text == "-") return 0;
    static const std::pair<const char*, std::uint16_t> kNames[] = {
        {"SELECT", padbtn::SELECT}, {"L3", padbtn::L3},       {"R3", padbtn::R3},
        {"START", padbtn::START},   {"UP", padbtn::UP},       {"RIGHT", padbtn::RIGHT},
        {"DOWN", padbtn::DOWN},     {"LEFT", padbtn::LEFT},   {"L2", padbtn::L2},
        {"R2", padbtn::R2},         {"L1", padbtn::L1},       {"R1", padbtn::R1},
        {"TRIANGLE", padbtn::TRIANGLE}, {"CIRCLE", padbtn::CIRCLE}, {"CROSS", padbtn::CROSS},
        {"SQUARE", padbtn::SQUARE},
    };
    std::uint16_t bits = 0;
    std::stringstream ss(text);
    std::string name;
    while (std::getline(ss, name, '+')) {
        bool ok = false;
        for (const auto& [n, b] : kNames) {
            if (name == n) {
                bits |= b;
                ok = true;
            }
        }
        if (!ok) throw anyps2::Error("botão de controle desconhecido: '" + name + "'");
    }
    return bits;
}

std::string padAxisName(const std::string& bound, int& dir) {
    dir = 0;
    if (!bound.empty() && (bound[0] == '+' || bound[0] == '-')) {
        dir = bound[0] == '+' ? 1 : -1;
        return bound.substr(1);
    }
    return bound;
}

bool padAxisPressed(int dir, int value) {
    constexpr int kThreshold = 8000;
    if (dir > 0) return value > kThreshold;
    if (dir < 0) return value < -kThreshold;
    return value > kThreshold || value < -kThreshold;
}

int padButtonIndex(const std::string& name) {
    for (int i = 0; i < 16; ++i) {
        if (name == kPadButtonNames[i]) return i;
    }
    return -1;
}

PadBindings PadBindings::defaults() {
    PadBindings b;
    // Teclado: setas, Z X A S = ✕ ○ □ △, Q/W = L1/R1, 1/2 = L2/R2; só a porta 1.
    const std::pair<const char*, const char*> keys[] = {
        {"up", "Up"},     {"down", "Down"},   {"left", "Left"},   {"right", "Right"}, {"cross", "Z"},
        {"circle", "X"},  {"square", "A"},    {"triangle", "S"},  {"l1", "Q"},        {"r1", "W"},
        {"l2", "1"},      {"r2", "2"},        {"start", "Return"}, {"select", "Backspace"},
    };
    for (const auto& [button, key] : keys) b.key[0][padButtonIndex(button)] = key;
    // Controle SDL: o mesmo para as duas portas; os gatilhos são eixos
    // (L2/R2 digitais acima de um limiar, como antes).
    const std::pair<const char*, const char*> pads[] = {
        {"cross", "a"},     {"circle", "b"},     {"square", "x"},      {"triangle", "y"},
        {"select", "back"}, {"start", "start"},  {"l3", "leftstick"},  {"r3", "rightstick"},
        {"l1", "leftshoulder"}, {"r1", "rightshoulder"}, {"up", "dpup"}, {"down", "dpdown"},
        {"left", "dpleft"}, {"right", "dpright"}, {"l2", "lefttrigger"}, {"r2", "righttrigger"},
    };
    const std::pair<const char*, const char*> analogs[] = {
        {"lx", "leftx"}, {"ly", "lefty"}, {"rx", "rightx"}, {"ry", "righty"},
    };
    for (unsigned port = 0; port < 2; ++port) {
        for (const auto& [button, name] : pads) b.button[port][padButtonIndex(button)] = name;
        for (int i = 0; i < 4; ++i) b.analog[port][i] = analogs[i].second;
    }
    return b;
}

void Input::setHost(unsigned port, const PadInput& state) {
    if (port > 1) return;
    std::lock_guard lock(mutex_);
    host_[port] = state;
}

void ds2Report(const PadInput& in, std::uint8_t out[18]) {
    const auto btns = static_cast<std::uint16_t>(~in.buttons);  // ativo em 0
    out[0] = static_cast<std::uint8_t>(btns);
    out[1] = static_cast<std::uint8_t>(btns >> 8);
    out[2] = in.rx;
    out[3] = in.ry;
    out[4] = in.lx;
    out[5] = in.ly;
    static const std::uint16_t kPressOrder[12] = {padbtn::RIGHT,    padbtn::LEFT,   padbtn::UP,    padbtn::DOWN,
                                                  padbtn::TRIANGLE, padbtn::CIRCLE, padbtn::CROSS, padbtn::SQUARE,
                                                  padbtn::L1,       padbtn::R1,     padbtn::L2,    padbtn::R2};
    for (int i = 0; i < 12; ++i) out[6 + i] = (in.buttons & kPressOrder[i]) ? 0xFF : 0x00;
}

PadInput Input::sample(unsigned port, std::uint64_t vblank) {
    if (!scripted_) {
        std::lock_guard lock(mutex_);
        return port < 2 ? host_[port] : PadInput{};
    }
    PadInput s;
    for (const auto& step : script_) {
        if (step.vblank > vblank) break;
        if (step.port == port) s = step.state;
    }
    return s;
}

void Input::loadScript(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw anyps2::Error("ANYPS2_PAD_SCRIPT: não foi possível ler " + path);
    std::string line;
    unsigned lineNo = 0;
    while (std::getline(f, line)) {
        ++lineNo;
        const auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        std::stringstream ss(line);
        Step step;
        std::string buttons;
        if (!(ss >> step.vblank)) continue;  // linha vazia
        if (!(ss >> step.port >> buttons) || step.port > 1) {
            throw anyps2::Error(path + ":" + std::to_string(lineNo) + ": esperado \"vblank porta botões\"");
        }
        if (buttons == "unplug") {
            step.state.connected = false;
        } else {
            step.state.buttons = parsePadButtons(buttons);
            unsigned axes[4];
            if (ss >> axes[0]) {
                if (!(ss >> axes[1] >> axes[2] >> axes[3]) || axes[0] > 255 || axes[1] > 255 || axes[2] > 255 ||
                    axes[3] > 255) {
                    throw anyps2::Error(path + ":" + std::to_string(lineNo) + ": analógicos são 4 valores 0..255");
                }
                step.state.lx = static_cast<std::uint8_t>(axes[0]);
                step.state.ly = static_cast<std::uint8_t>(axes[1]);
                step.state.rx = static_cast<std::uint8_t>(axes[2]);
                step.state.ry = static_cast<std::uint8_t>(axes[3]);
            }
        }
        script_.push_back(step);
    }
    std::stable_sort(script_.begin(), script_.end(),
                     [](const Step& a, const Step& b) { return a.vblank < b.vblank; });
    scripted_ = true;
}

}  // namespace anyps2::rt
