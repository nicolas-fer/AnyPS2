#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace anyps2::rt {

class Iop;

// audsrv.irx (ps2sdk) em HLE: servidor RPC 0x0870884E.
//
// PCM: o EE envia amostras (8/16 bits, mono/estéreo, 11025..48000 Hz) para
// um buffer circular do tamanho do original (~107 ms); o mixer do IOP o
// consome no ritmo do tempo emulado, convertendo para 48 kHz como o
// audsrv real (amostra mais próxima). audsrv_wait_audio bloqueia o EE (a
// resposta RPC é adiada) até haver espaço.
//
// ADPCM: audsrv_load_adpcm copia o sample para a RAM do SPU2 e
// audsrv_ch_play_adpcm o toca numa voz do núcleo 1.
//
// Não suportado (erro claro): faixas de CD-DA (audsrv_play_cd & cia.) e o
// callback audsrv_on_fillbuf, que exigiria o IOP chamar um servidor RPC do
// EE.
class AudSrv {
public:
    explicit AudSrv(Iop& iop);
    void registerServer();
    void reset();
    // Soma o PCM em `mix` (frames × 2, 48 kHz), consumindo o buffer.
    void render(std::int32_t* mix, std::size_t frames);
    // Depois de mixar: libera quem espera espaço no buffer.
    void update(std::uint32_t pc);

private:
    struct Sample {
        std::uint32_t spuAddr = 0, size = 0, pitch = 0, loop = 0, channels = 0;
    };
    struct Wait {
        std::uint32_t token;
        std::uint32_t bytes;
    };
    std::optional<std::vector<std::uint8_t>> rpc(std::uint32_t fn, const std::vector<std::uint8_t>& in,
                                                 std::uint32_t pc);
    std::int32_t setFormat(std::uint32_t freq, std::uint32_t bits, std::uint32_t channels);
    std::uint32_t available() const { return ringSize_ - static_cast<std::uint32_t>(ring_.size()); }
    std::uint32_t frameBytes() const { return (bits_ / 8) * channels_; }

    Iop& iop_;
    bool initialized_ = false, playing_ = false;
    std::uint32_t freq_ = 48000, bits_ = 16, channels_ = 2;
    std::int32_t volume_ = 0x3FFF;
    std::deque<std::uint8_t> ring_;
    std::uint32_t ringSize_ = 20480;
    std::uint64_t phase_ = 0;  // posição de leitura em 1/48000 de amostra da origem
    std::int32_t curL_ = 0, curR_ = 0;
    bool haveFrame_ = false;
    std::vector<Wait> waits_;
    std::map<std::uint32_t, Sample> samples_;  // id (ponteiro no EE) -> sample
    std::uint32_t nextSpuAddr_ = 0x5010;
    std::array<std::pair<std::uint16_t, std::uint16_t>, 24> adpcmVol_;
};

}  // namespace anyps2::rt
