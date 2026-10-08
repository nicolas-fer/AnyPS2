#include "anyps2/runtime/iop/pad.h"

#include <cstdio>
#include <cstring>

#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/memory.h"
#include "anyps2/runtime/runtime.h"

namespace anyps2::rt {

using namespace iopio;

namespace {

// Comandos (numeração do XPADMAN; os do PADMAN da ROM são 0x80000100 + n - 1).
enum : std::uint32_t {
    CMD_OPEN = 0x01, CMD_INFO_ACT = 0x03, CMD_INFO_COMB, CMD_INFO_MODE, CMD_SET_MMODE, CMD_SET_ACTDIR,
    CMD_SET_ACTALIGN, CMD_GET_BTNMASK, CMD_SET_BTNINFO, CMD_SET_VREF, CMD_GET_PORTMAX, CMD_GET_SLOTMAX,
    CMD_CLOSE, CMD_END, CMD_INIT, CMD_GET_MODVER = 0x12,
};
constexpr std::uint8_t kStateDisconnected = 0, kStateStable = 6;
constexpr std::uint8_t kReqComplete = 0, kReqBusy = 2;
constexpr std::uint8_t kTaskUpdatePad = 1;
constexpr std::uint8_t kModeConfigReady = 2;

// Tabela de modos e atuadores de um DualShock 2.
constexpr std::uint16_t kModeTable[2] = {0x0004, 0x0007};  // digital, DualShock
constexpr std::uint8_t kActData[2][4] = {{0, 1, 0, 10}, {0, 1, 1, 20}};

}  // namespace

PadMan::PadMan(Iop& iop) : iop_(iop) {}

void PadMan::reset() {
    slots_ = {};
    statBuf_ = statFrame_ = 0;
}

void PadMan::load(bool newProtocol) {
    newProtocol_ = newProtocol;
    auto handler = [this](std::uint32_t fn, const std::vector<std::uint8_t>& in, std::uint32_t pc) {
        return rpc(fn, in, pc);
    };
    auto ext = [](std::uint32_t, const std::vector<std::uint8_t>& in, std::uint32_t) {
        return std::optional<std::vector<std::uint8_t>>(in);  // "Extend Service" do padman: sem uso
    };
    if (newProtocol) {
        iop_.registerServer(0x80000100u, "padman", handler);
        iop_.registerServer(0x80000101u, "padman-ext", ext);
    } else {
        iop_.registerServer(0x8000010Fu, "padman-rom", handler);
        iop_.registerServer(0x8000011Fu, "padman-rom-ext", ext);
    }
}

std::optional<std::vector<std::uint8_t>> PadMan::rpc(std::uint32_t, const std::vector<std::uint8_t>& in,
                                                     std::uint32_t pc) {
    std::vector<std::uint8_t> d = in;
    d.resize(std::max<std::size_t>(d.size(), 128));
    std::uint32_t cmd = rd32(d, 0);
    if (cmd & 0x80000000u) cmd = (cmd & 0xFF) + 1;  // PADMAN da ROM
    const std::uint32_t port = rd32(d, 4), slot = rd32(d, 8);
    auto valid = [&] { return port < 2 && slot == 0; };
    auto set = [&](unsigned word, std::int32_t v) { wr32(d, word * 4, static_cast<std::uint32_t>(v)); };
    if (iop_.runtime().options().traceIop) std::fprintf(stderr, "[iop] padman cmd %u porta %u\n", cmd, port);
    switch (cmd) {
        case CMD_INIT:  // {cmd, -, -, result, statBuf}
            statBuf_ = rd32(d, 16);
            set(3, 1);
            return d;
        case CMD_END:
            slots_ = {};
            statBuf_ = 0;
            set(3, 1);
            return d;
        case CMD_GET_MODVER:
            set(3, 0x0306);
            return d;
        case CMD_GET_PORTMAX:
            set(3, 2);
            return d;
        case CMD_GET_SLOTMAX:
            set(3, 1);
            return d;
        case CMD_OPEN: {  // {cmd, port, slot, result, padArea, padBuf}
            if (!valid()) {
                set(3, 0);
                return d;
            }
            Slot& s = slots_[port];
            s = Slot{};
            s.open = true;
            s.area = rd32(d, 16);
            set(3, 1);
            set(5, static_cast<std::int32_t>(0x00180000u + port * 0x100u));  // estado no IOP (opaco)
            return d;
        }
        case CMD_CLOSE:
            if (valid()) slots_[port].open = false;
            set(3, 1);
            return d;
        case CMD_INFO_MODE: {  // {cmd, port, slot, term, offs, result}
            const auto term = static_cast<std::int32_t>(rd32(d, 12));
            const auto offs = static_cast<std::int32_t>(rd32(d, 16));
            std::int32_t r = -1;
            if (!valid() || !slots_[port].open) r = 0;
            else if (slots_[port].reqState == kReqBusy) r = 0;
            else if (term == 1) r = slots_[port].modeCurId >> 4;      // PAD_MODECURID
            else if (term == 2) r = kModeTable[slots_[port].analog ? 1 : 0];  // PAD_MODECUREXID
            else if (term == 3) r = slots_[port].analog ? 1 : 0;      // PAD_MODECUROFFS
            else if (term == 4) r = offs == -1 ? 2 : (offs >= 0 && offs < 2 ? kModeTable[offs] : 0);
            set(5, r);
            return d;
        }
        case CMD_SET_MMODE: {  // {cmd, port, slot, mode, lock, result}
            const std::uint32_t mode = rd32(d, 12), lock = rd32(d, 16);
            std::int32_t r = 0;
            if (valid() && slots_[port].open && mode < 2 && slots_[port].reqState != kReqBusy) {
                Slot& s = slots_[port];
                s.pending = true;
                s.pendingAnalog = mode == 1;
                s.pendingPressure = s.pressure && mode == 1;
                s.lock = lock == 3;
                s.reqState = kReqBusy;
                r = 1;
            }
            set(5, r);
            return d;
        }
        case CMD_GET_BTNMASK:
            set(3, valid() ? 0x3FFFF : 0);
            return d;
        case CMD_SET_BTNINFO: {  // {cmd, port, slot, info, result}
            std::int32_t r = 0;
            if (valid() && slots_[port].open && slots_[port].reqState != kReqBusy) {
                Slot& s = slots_[port];
                s.pending = true;
                s.pendingAnalog = s.analog;
                s.pendingPressure = rd32(d, 12) != 0 && s.analog;
                s.reqState = kReqBusy;
                r = 1;
            }
            set(4, r);
            return d;
        }
        case CMD_INFO_ACT: {  // {cmd, port, slot, act, val, result}
            const auto act = static_cast<std::int32_t>(rd32(d, 12));
            const std::uint32_t val = rd32(d, 16);
            std::int32_t r = -1;
            if (act == -1) r = 2;
            else if (act >= 0 && act < 2 && val >= 1 && val <= 4) r = kActData[act][val - 1];
            set(5, r);
            return d;
        }
        case CMD_INFO_COMB:
            set(5, static_cast<std::int32_t>(rd32(d, 12)) == -1 ? 0 : -1);
            return d;
        case CMD_SET_ACTDIR:
        case CMD_SET_ACTALIGN: {  // {cmd, port, slot, u8 data[6], ..., result em [5]}
            if (valid()) {
                auto& dst = cmd == CMD_SET_ACTDIR ? slots_[port].actDirect : slots_[port].actAlign;
                std::memcpy(dst.data(), d.data() + 12, 6);
            }
            set(5, valid() ? 1 : 0);
            return d;
        }
        case CMD_SET_VREF:
            set(7, 1);
            return d;
        default:
            throw Unimplemented("padman: comando " + anyps2::hex(rd32(d, 0)) + " não implementado no HLE", pc);
    }
}

void PadMan::vblank(std::uint32_t pc) {
    ++vblanks_;
    for (unsigned port = 0; port < 2; ++port) {
        if (slots_[port].open) writeState(port, slots_[port], pc);
    }
    if (statBuf_) {
        // Estrutura de "portas abertas" do XPADMAN: {frame, openSlots[2], ...}.
        ++statFrame_;
        std::vector<std::uint8_t> st(128, 0);
        wr32(st, 0, statFrame_);
        wr32(st, 4, slots_[0].open ? 1 : 0);
        wr32(st, 8, slots_[1].open ? 1 : 0);
        iop_.runtime().memory().copyToGuest(statBuf_ + (statFrame_ % 2 == 0 ? 128 : 0), st.data(), 128, pc);
    }
}

void PadMan::writeState(unsigned port, Slot& s, std::uint32_t pc) {
    const PadInput in = iop_.runtime().input().sample(port, vblanks_);
    if (s.pending) {  // pedido do EE concluído neste ciclo do padman
        s.pending = false;
        s.analog = s.pendingAnalog;
        s.pressure = s.pendingPressure;
        s.reqState = kReqComplete;
    }
    s.modeCurId = !s.analog ? 0x41 : s.pressure ? 0x79 : 0x73;
    ++s.frame;

    // Bytes de botões como o padman entrega ao EE (padButtonStatus).
    std::uint8_t data[32];
    std::memset(data, 0, sizeof(data));
    data[0] = 0;
    data[1] = s.modeCurId;
    ds2Report(in, data + 2);
    const bool connected = in.connected;
    const std::uint32_t length = connected ? 2u + 2u * (s.modeCurId & 0x0Fu) : 0u;
    const std::uint8_t state = connected ? kStateStable : kStateDisconnected;

    Memory& m = iop_.runtime().memory();
    if (newProtocol_) {
        std::vector<std::uint8_t> p(128, 0);  // struct pad_data_new
        std::memcpy(p.data(), data, 32);
        std::memcpy(p.data() + 32, s.actDirect.data(), 6);
        std::memcpy(p.data() + 40, s.actAlign.data(), 6);
        for (int a = 0; a < 2; ++a) std::memcpy(p.data() + 48 + a * 4, kActData[a], 4);
        p[80] = static_cast<std::uint8_t>(kModeTable[0]);
        p[82] = static_cast<std::uint8_t>(kModeTable[1]);
        wr32(p, 88, s.frame);
        wr32(p, 96, length);
        p[100] = connected ? kModeConfigReady : 0;
        p[101] = connected ? s.modeCurId : 0;
        p[102] = 3;  // modelo: DualShock 2
        p[103] = connected ? 1 : 0;  // buttonDataReady
        p[104] = 2;  // nrOfModes
        p[105] = s.analog ? 1 : 0;
        p[106] = 2;  // atuadores
        p[109] = s.analog ? 1 : 0;
        p[110] = s.lock ? 3 : 0;
        p[111] = 6;
        p[112] = state;
        p[113] = s.reqState;
        p[114] = kTaskUpdatePad;
        m.copyToGuest(s.area + (s.frame % 2 == 0 ? 128 : 0), p.data(), 128, pc);
    } else {
        std::vector<std::uint8_t> p(64, 0);  // struct pad_data_old
        wr32(p, 0, s.frame);
        p[4] = state;
        p[5] = s.reqState;
        p[6] = connected ? 1 : 0;
        std::memcpy(p.data() + 8, data, 32);
        wr32(p, 40, length);
        p[45] = 2;  // CTP: com configuração
        p[46] = 3;
        m.copyToGuest(s.area + (s.frame % 2 == 0 ? 64 : 0), p.data(), 64, pc);
    }
}

}  // namespace anyps2::rt
