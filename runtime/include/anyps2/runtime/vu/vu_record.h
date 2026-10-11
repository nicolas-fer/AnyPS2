#pragma once

// Gravação e replay das execuções do VU1 (ANYPS2_VU_RECORD), para medir e
// validar mudanças no runtime/gerador do VU sem rodar o jogo: cada execução
// (run) guarda o estado completo de entrada, e o replay reaplica esse estado
// num Vu sem Runtime, executa de novo e resume o estado final num hash.
//
// Variáveis de ambiente (VuRecorder::fromEnv):
//   ANYPS2_VU_RECORD=arquivo     liga a gravação (o arquivo é recriado)
//   ANYPS2_VU_RECORD_FRAMES=a-b  VBlanks gravados (inclusivo). "a" = só a; "a-" = de a em diante;
//                                "-b" = até b. Sem ela grava tudo (cerca de 16 KB por execução:
//                                use um intervalo curto, 2 a 3 VBlanks).
//
// Formato do arquivo (versão 1; inteiros little-endian, sem alinhamento):
//   cabeçalho: "AVUR" | u32 versão (=1) | u32 microSize | u32 dataSize | u32 unidade
//   registros, cada um começando por u8 tipo:
//     1 = imagem da micro memória: u64 hash (hashBytes de vu_record.cpp) | microSize bytes. Só é gravada quando
//         muda; as entradas a referenciam pelo hash.
//     2 = entrada (uma execução):
//         u64 vblank | u32 startPc | u32 eePc | u64 microHash | u32 vifTop | u32 vifItop | u64 ciclo
//         32 x 16 bytes VF | 32 x u32 VI (vi[16..] são os de controle: status, MAC, clip, R, I, Q, P...)
//         | 16 bytes ACC
//         pipeline (formato do VuPipelineSeed, atrasos relativos ao ciclo da entrada):
//           31 x u8 vfBusy (VF1..VF31) | u32 cabeça do anel | u8 n | n x {u8 delay, u8 hasMac,
//           u16 mac, u8 hasClip, u32 clip} | Q: u8 ativo, u8 delay, u32 valor, u32 flags | P: idem
//         | dataSize bytes da memória de dados
// O estado gravado é o do início de Vu::run, antes de qualquer par; a memória de dados que o EE/VIF
// escreveu entre execuções já está nele, por isso cada entrada se reproduz sozinha.

#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt {

inline constexpr std::uint32_t kVuRecVersion = 1;

class VuRecorder {
public:
    // Lê ANYPS2_VU_RECORD e ANYPS2_VU_RECORD_FRAMES. Nulo sem a primeira ou se o arquivo não abre.
    static std::unique_ptr<VuRecorder> fromEnv();

    // Abre (recriando) o arquivo; só grava VBlanks em [from, to].
    bool open(const std::string& path, std::uint64_t from = 0, std::uint64_t to = ~std::uint64_t{0});
    bool active() const { return out_.is_open(); }
    // Gancho de Vu::run: grava a entrada se o VBlank estiver no intervalo.
    void record(Vu& vu, std::uint32_t startPc, std::uint32_t eePc, std::uint64_t vblank);
    void close() { out_.close(); }
    std::uint64_t entries() const { return entries_; }

private:
    std::ofstream out_;
    std::uint64_t from_ = 0, to_ = ~std::uint64_t{0};
    bool headerDone_ = false;
    std::uint64_t entries_ = 0;
    std::vector<std::uint64_t> micros_;  // hashes das imagens já gravadas
};

// Uma entrada lida do arquivo.
struct VuRecEntry {
    std::uint64_t vblank = 0, cycle = 0, microHash = 0;
    std::uint32_t startPc = 0, eePc = 0, vifTop = 0, vifItop = 0;
    Reg128 vf[32]{};
    std::uint32_t vi[32]{};
    Reg128 acc{};
    VuPipelineSeed pipe;
    std::vector<std::uint8_t> data;
    std::shared_ptr<const std::vector<std::uint8_t>> micro;
};

struct VuRecording {
    std::uint32_t microSize = 0, dataSize = 0, unit = 0;
    std::vector<VuRecEntry> entries;
    // Lança anyps2::Error se o arquivo não abrir ou estiver corrompido/de outra versão.
    static VuRecording load(const std::string& path);
};

// Resumo do estado final de uma execução. As seções permitem dizer o que diferiu.
struct VuRunHash {
    std::uint64_t data = 0;    // memória de dados
    std::uint64_t vec = 0;     // VF e ACC
    std::uint64_t ctl = 0;     // vi[0..31]: inteiros, status/MAC/clip, R/I/Q/P, TPC
    std::uint64_t pipe = 0;    // pipeline ao terminar (VF ocupados, filas), relativo ao ciclo
    std::uint64_t kicks = 0;   // endereço e ciclo relativo de cada XGKICK
    std::uint64_t pairs = 0;   // pares executados
    std::uint64_t cycles = 0;  // ciclos gastos
    std::uint64_t all() const;
    bool operator==(const VuRunHash&) const = default;
    // "all data vec ctl pipe kicks pairs cycles" em hexadecimal/decimal, uma linha.
    std::string str() const;
    static bool parse(const std::string& line, VuRunHash& out);
    // Nomes das seções que diferem de o.
    std::string diff(const VuRunHash& o) const;
};

// Hash do estado de um Vu logo depois de uma execução que começou no ciclo `cycle0` e registrou os
// XGKICK em `kicks`. Serve igualmente à execução direta e ao replay.
VuRunHash hashVuRun(Vu& vu, std::uint64_t cycle0, const std::vector<VuXgkickEvent>& kicks);

// Vu sem Runtime que reaplica as entradas gravadas.
class VuReplayMachine {
public:
    VuReplayMachine(std::uint32_t microSize, std::uint32_t dataSize, unsigned unit = 1);
    VuReplayMachine(const VuReplayMachine&) = delete;
    VuReplayMachine& operator=(const VuReplayMachine&) = delete;

    Vu& vu() { return vu_; }
    // Aplica o estado de entrada (memória, registradores, pipeline, TOP/ITOP).
    void apply(const VuRecEntry& e);
    // Executa a entrada já aplicada (pelo interpretador se `interpret`). O resumo só é calculado
    // com `hash`; `ns` recebe o tempo gasto só na execução (se não for nulo). Um erro do guest
    // (exceção) é devolvido em `error` e o hash cobre o estado até ali.
    VuRunHash run(const VuRecEntry& e, bool interpret, bool hash, std::uint64_t* ns, std::string* error = nullptr);
    // apply + run.
    VuRunHash replay(const VuRecEntry& e, bool interpret, bool hash, std::uint64_t* ns, std::string* error = nullptr) {
        apply(e);
        return run(e, interpret, hash, ns, error);
    }

private:
    Reg128 vf_[32]{};
    std::uint32_t vi_[32]{};
    Reg128 acc_{};
    std::vector<std::uint8_t> data_, micro_;
    std::vector<VuXgkickEvent> kicks_;
    const std::vector<std::uint8_t>* lastMicro_ = nullptr;
    Vu vu_;
};

}  // namespace anyps2::rt
