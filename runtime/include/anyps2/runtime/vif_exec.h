#pragma once

#include <cstddef>
#include <cstdint>

namespace anyps2::rt {

class Runtime;
class Gif;
struct VuMemory;

// Itens de execução do VIF (fatia 3 do plano do VU1 em outra thread). A decodificação do fluxo de
// VIFcodes (Vif, no EE) não toca a memória do VU, não chama o VU nem o GIF: ela produz estes
// itens, e o executor (VifExec) os aplica. Hoje o executor roda inline, na hora e na ordem da
// decodificação, então o resultado é o mesmo de antes da separação; com o VU1 em outra thread, os
// itens passam a ir para a fila, na mesma ordem.
//
// Os ponteiros do item só valem durante a chamada de apply (apontam para buffers do decodificador);
// uma fila precisa copiar os dados. Tudo o que o executor e o VU precisam saber do estado do VIF
// vem no item (endereço de destino já resolvido com FLG/TOPS, máscara/ROW/COL/MODE da época,
// TOP/ITOP, VBlank do rastreador), nunca de uma consulta ao Vif durante a execução.
//
// UNPACK: escolheu-se a lista de quadwords já montados (destino + 4 componentes + máscara de
// escrita), e não os dados crus com CL/WL/máscara. O ROW com MODE 1/2 acumula a cada vetor, então
// quem mantém o ROW é o decodificador (a leitura de ROW pelo EE não precisa de fence), e o
// executor fica sem estado: só grava os componentes liberados, em ordem. O custo é 24 bytes por
// quadword em vez dos 4 a 16 bytes crus, pagos uma vez na cópia para a fila.
struct VifQwWrite {
    std::uint32_t qw;       // quadword de destino, já reduzido ao tamanho da memória de dados
    std::uint32_t mask;     // bit c = grava o componente c (os demais ficam como estão)
    std::uint32_t v[4];
};

struct VifItem {
    enum class Kind : std::uint8_t {
        Unpack,     // writes[0..count)
        Mpg,        // words[0..count) a partir do endereço addr (bytes) da micro memória
        Mscal,      // MSCAL/MSCALF: começa em start (bytes)
        Mscnt,      // MSCNT: continua do TPC do VU (resolvido no executor, sem fence)
        Direct,     // 16 bytes em data, para o GIF pelo PATH2
        Path3Mask,  // MSKPATH3: masked
        Reset,      // FBRST.RST: PATH3 volta a não mascarado, TOP/ITOP do VU1 voltam a zero
    };
    explicit VifItem(Kind k) : kind(k) {}
    Kind kind;
    std::uint32_t pc = 0;  // pc do EE da transferência (para mensagens de erro e do GIF)
    // Unpack
    const VifQwWrite* writes = nullptr;
    // Mpg
    const std::uint32_t* words = nullptr;
    std::uint32_t addr = 0;
    // Unpack e Mpg
    std::size_t count = 0;
    // Mscal/Mscnt: snapshots da época do comando (substituem a leitura de TOP/ITOP e do VBlank
    // durante a execução do VU)
    std::uint32_t start = 0, top = 0, itop = 0;
    std::uint64_t vblanks = 0;
    // Direct
    const std::uint8_t* data = nullptr;
    // Path3Mask
    bool masked = false;
};

// Aplica os itens de uma unidade (0 ou 1) do VIF na memória do VU, no VU e no GIF. Sem estado
// próprio além das referências; genérico por unidade (o VIF0 não gera Direct nem Path3Mask).
class VifExec {
public:
    VifExec(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif);
    void apply(const VifItem& item);

private:
    Runtime* rt_;
    unsigned unit_;
    VuMemory& vu_;
    Gif* gif_;
};

}  // namespace anyps2::rt
