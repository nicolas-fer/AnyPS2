// Configuração do host (anyps2.ini, ANYPS2_CONFIG): leitura do arquivo INI,
// erros de sintaxe com linha, prioridade padrão < arquivo < ambiente e o
// mapeamento de teclas e controle para os botões do DS2.

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>

#include "anyps2/runtime/config.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/runtime.h"
#include "minitest.h"

using namespace anyps2::rt;

namespace {

using Vars = std::map<std::string, std::string>;

// Fonte de variáveis de ambiente a partir de um mapa (o mapa precisa viver
// enquanto a função for usada).
std::function<const char*(const char*)> envOf(const Vars& vars) {
    return [&vars](const char* name) -> const char* {
        const auto it = vars.find(name);
        return it == vars.end() ? nullptr : it->second.c_str();
    };
}

// Escreve um arquivo temporário e devolve o caminho.
std::string writeTemp(const std::string& name, const std::string& text) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary);
    out << text;
    out.close();
    return path.string();
}

// Caminho que certamente não existe: "sem arquivo de configuração".
const char* kNoFile = "anyps2_nao_existe_config.ini";

}  // namespace

TEST_CASE(config, file_values) {
    RuntimeOptions o;
    applyConfigText(o,
                    "[memcard]\n"
                    "dir = C:\\cartoes\n"
                    "\n"
                    "[disc]\n"
                    "iso = C:\\jogos\\gt4.iso\n"
                    "[video]\n"
                    "mode = none\n"
                    "scale = 3\n"
                    "[audio]\n"
                    "mode = AUTO\n"
                    "wav = som.wav\n",
                    "anyps2.ini");
    CHECK_EQ(o.memcardDir, std::string("C:\\cartoes"));
    CHECK_EQ(o.iso, std::string("C:\\jogos\\gt4.iso"));
    CHECK_EQ(o.video, std::string("none"));
    CHECK_EQ(o.videoScale, 3);
    CHECK_EQ(o.audio, std::string(""));  // auto
    CHECK_EQ(o.audioWav, std::string("som.wav"));
}

TEST_CASE(config, comments_and_spacing) {
    RuntimeOptions o;
    // Linha inteira com # ou ;, comentário depois de espaço, CRLF, "#" colado
    // num valor (faz parte dele) e chave com espaços em volta do "=".
    applyConfigText(o,
                    "# anyps2.ini de teste\r\n"
                    "; outro comentário\r\n"
                    "[  video  ]\r\n"
                    "  scale   =   2   # janela dupla\r\n"
                    "\r\n"
                    "[memcard]\r\n"
                    "dir = C:\\a#b\n",
                    "anyps2.ini");
    CHECK_EQ(o.videoScale, 2);
    CHECK_EQ(o.memcardDir, std::string("C:\\a#b"));
}

TEST_CASE(config, syntax_errors_name_the_line) {
    RuntimeOptions o;
    CHECK_THROWS_WITH(applyConfigText(o, "[video]\nmode = sdl\nscale = 9\n", "x.ini"),
                      "x.ini:3: valor inválido para [video] scale: '9'");
    CHECK_THROWS_WITH(applyConfigText(o, "[seçao]\n", "x.ini"), "x.ini:1: seção desconhecida [seçao]");
    CHECK_THROWS_WITH(applyConfigText(o, "mode = sdl\n", "x.ini"), "x.ini:1: 'mode' está fora de qualquer seção");
    CHECK_THROWS_WITH(applyConfigText(o, "[video\n", "x.ini"), "x.ini:1: seção sem ']' no fim");
    CHECK_THROWS_WITH(applyConfigText(o, "[video]\nmode sdl\n", "x.ini"),
                      "x.ini:2: esperado 'chave = valor' ou [seção]");
    CHECK_THROWS_WITH(applyConfigText(o, "[video]\nmode = qualquer\n", "x.ini"),
                      "x.ini:2: valor inválido para [video] mode: 'qualquer'");
    CHECK_THROWS_WITH(applyConfigText(o, "[video]\nfoo = 1\n", "x.ini"), "x.ini:2: chave desconhecida 'foo'");
    CHECK_THROWS_WITH(applyConfigText(o, "[gamepad.2]\n\nfoo = a\n", "x.ini"),
                      "x.ini:3: botão desconhecido 'foo' em [gamepad.2]");
    CHECK_THROWS_WITH(applyConfigText(o, "[keyboard.1]\nlx = Q\n", "x.ini"),
                      "x.ini:2: 'lx' é analógico e não tem tecla: ligue-o em [gamepad.1]");
    CHECK_THROWS_WITH(applyConfigText(o, "[video]\nscale = 0\n", "x.ini"), "x.ini:2: valor inválido para [video] scale");
}

TEST_CASE(config, priority_env_over_file_over_default) {
    const std::string path = writeTemp("anyps2_test_prioridade.ini",
                                       "[video]\nmode = none\nscale = 2\n"
                                       "[disc]\niso = do_arquivo.iso\n"
                                       "[memcard]\ndir = cartoes_arquivo\n");
    // Só o arquivo (ANYPS2_CONFIG aponta para ele): vale o arquivo.
    Vars vars{{"ANYPS2_CONFIG", path}};
    RuntimeOptions fromFile = resolveOptions(envOf(vars), kNoFile);
    CHECK_EQ(fromFile.video, std::string("none"));
    CHECK_EQ(fromFile.videoScale, 2);
    CHECK_EQ(fromFile.iso, std::string("do_arquivo.iso"));
    CHECK_EQ(fromFile.memcardDir, std::string("cartoes_arquivo"));

    // Ambiente sobrepõe o arquivo, campo a campo; o que o ambiente não define
    // continua vindo do arquivo.
    vars["ANYPS2_ISO"] = "do_ambiente.iso";
    vars["ANYPS2_VIDEO_SCALE"] = "4";
    vars["ANYPS2_MC_DIR"] = "cartoes_ambiente";
    RuntimeOptions mixed = resolveOptions(envOf(vars), kNoFile);
    CHECK_EQ(mixed.iso, std::string("do_ambiente.iso"));
    CHECK_EQ(mixed.videoScale, 4);
    CHECK_EQ(mixed.memcardDir, std::string("cartoes_ambiente"));
    CHECK_EQ(mixed.video, std::string("none"));

    // ANYPS2_VIDEO vazia é "automático" e vence o "none" do arquivo.
    vars["ANYPS2_VIDEO"] = "";
    CHECK_EQ(resolveOptions(envOf(vars), kNoFile).video, std::string(""));

    // Sem arquivo (padrão inexistente) e sem ambiente: o padrão de sempre.
    Vars none;
    RuntimeOptions defaults = resolveOptions(envOf(none), kNoFile);
    CHECK_EQ(defaults.video, std::string(""));
    CHECK_EQ(defaults.audio, std::string(""));
    CHECK_EQ(defaults.iso, std::string(""));
    CHECK_EQ(defaults.memcardDir, std::string(""));
    CHECK_EQ(defaults.videoScale, 1);
    std::filesystem::remove(path);
}

TEST_CASE(config, defaults_match_the_fixed_mapping_of_before) {
    const RuntimeOptions o = resolveOptions(envOf(Vars{}), kNoFile);
    // Teclado da porta 1, como era fixo na janela.
    CHECK_EQ(o.pad.key[0][padButtonIndex("up")], std::string("Up"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("cross")], std::string("Z"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("circle")], std::string("X"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("square")], std::string("A"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("triangle")], std::string("S"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("l1")], std::string("Q"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("r1")], std::string("W"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("l2")], std::string("1"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("r2")], std::string("2"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("start")], std::string("Return"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("select")], std::string("Backspace"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("l3")], std::string(""));
    // Teclado não tinha porta 2.
    for (int i = 0; i < 16; ++i) CHECK_EQ(o.pad.key[1][i], std::string(""));
    // Controle, as duas portas: gatilhos são eixos, sticks são analógicos.
    for (unsigned port = 0; port < 2; ++port) {
        CHECK_EQ(o.pad.button[port][padButtonIndex("cross")], std::string("a"));
        CHECK_EQ(o.pad.button[port][padButtonIndex("start")], std::string("start"));
        CHECK_EQ(o.pad.button[port][padButtonIndex("l2")], std::string("lefttrigger"));
        CHECK_EQ(o.pad.button[port][padButtonIndex("r3")], std::string("rightstick"));
        CHECK_EQ(o.pad.analog[port][0], std::string("leftx"));
        CHECK_EQ(o.pad.analog[port][3], std::string("righty"));
    }
}

TEST_CASE(config, key_and_button_mapping) {
    // A ordem de kPadButtonNames é a dos bits de padbtn: o índice i é o bit 1<<i.
    CHECK_EQ(1u << padButtonIndex("cross"), static_cast<unsigned>(padbtn::CROSS));
    CHECK_EQ(1u << padButtonIndex("select"), static_cast<unsigned>(padbtn::SELECT));
    CHECK_EQ(1u << padButtonIndex("square"), static_cast<unsigned>(padbtn::SQUARE));
    CHECK_EQ(padButtonIndex("nao_existe"), -1);

    RuntimeOptions o;
    applyConfigText(o,
                    "[keyboard.1]\n"
                    "cross = Space\n"
                    "up =\n"  // vazio desliga a tecla padrão
                    "[keyboard.2]\n"
                    "circle = Left Shift\n"
                    "[gamepad.2]\n"
                    "square = x\n"
                    "lx = rightx\n",
                    "anyps2.ini");
    CHECK_EQ(o.pad.key[0][padButtonIndex("cross")], std::string("Space"));
    CHECK_EQ(o.pad.key[0][padButtonIndex("up")], std::string(""));
    CHECK_EQ(o.pad.key[1][padButtonIndex("circle")], std::string("Left Shift"));
    CHECK_EQ(o.pad.button[1][padButtonIndex("square")], std::string("x"));
    CHECK_EQ(o.pad.analog[1][0], std::string("rightx"));
    // A porta 1 do controle não foi tocada.
    CHECK_EQ(o.pad.button[0][padButtonIndex("square")], std::string("x"));
    CHECK_EQ(o.pad.analog[0][0], std::string("leftx"));
}

TEST_CASE(config, missing_explicit_file_is_an_error) {
    Vars vars{{"ANYPS2_CONFIG", "anyps2_nao_existe_de_verdade.ini"}};
    CHECK_THROWS_WITH(resolveOptions(envOf(vars), kNoFile), "ANYPS2_CONFIG: não foi possível ler");
}

TEST_CASE(config, invalid_env_scale_is_an_error) {
    Vars vars{{"ANYPS2_VIDEO_SCALE", "7"}};
    CHECK_THROWS_WITH(resolveOptions(envOf(vars), kNoFile), "ANYPS2_VIDEO_SCALE='7' inválido");
}

TEST_CASE(config, axis_direction_is_explicit) {
    int dir = 99;
    CHECK_EQ(padAxisName("leftx", dir), std::string("leftx"));
    CHECK_EQ(dir, 0);
    CHECK_EQ(padAxisName("+leftx", dir), std::string("leftx"));
    CHECK_EQ(dir, 1);
    CHECK_EQ(padAxisName("-righty", dir), std::string("righty"));
    CHECK_EQ(dir, -1);
    // "+" só para um lado, "-" só para o outro; sem sinal, os dois.
    CHECK(padAxisPressed(1, 9000));
    CHECK(!padAxisPressed(1, -9000));
    CHECK(!padAxisPressed(1, 5000));
    CHECK(padAxisPressed(-1, -9000));
    CHECK(!padAxisPressed(-1, 9000));
    CHECK(padAxisPressed(0, 9000));
    CHECK(padAxisPressed(0, -9000));
    CHECK(!padAxisPressed(0, 8000));  // limiar: estritamente acima
    // Gatilho (0..32767) como antes: aciona acima do limiar, sem direção.
    CHECK(padAxisPressed(0, 20000));
    CHECK(!padAxisPressed(0, 0));
}

TEST_CASE(config, signed_axis_accepted_on_digital_button_only) {
    RuntimeOptions o;
    applyConfigText(o, "[gamepad.1]\nl2 = +lefttrigger\nlx = leftx\n", "x.ini");
    CHECK_EQ(o.pad.button[0][padButtonIndex("l2")], std::string("+lefttrigger"));
    CHECK_THROWS_WITH(applyConfigText(o, "[gamepad.1]\nlx = +leftx\n", "x.ini"),
                      "x.ini:2: eixo analógico não aceita sinal");
    CHECK_THROWS_WITH(applyConfigText(o, "[gamepad.1]\nsquare = +x\n", "x.ini"),
                      "x.ini:2: sinal (+ ou -) só vale para eixos");
}

TEST_CASE(config, default_file_is_next_to_the_executable) {
    const std::filesystem::path p = defaultConfigPath();
    CHECK_EQ(p.filename().string(), std::string("anyps2.ini"));
    CHECK(p.is_absolute());
    // Sem ANYPS2_CONFIG, o caminho usado fica registrado para o menu gravar nele.
    const RuntimeOptions o = resolveOptions(envOf(Vars{}), p.string());
    CHECK_EQ(o.configPath, p.string());
}

#if defined(ANYPS2_SDL_NAMES)
TEST_CASE(config, unknown_names_are_errors_with_line_and_section) {
    RuntimeOptions o;
    CHECK_THROWS_WITH(applyConfigText(o, "[keyboard.1]\n\ncross = Zz\n", "anyps2.ini"),
                      "anyps2.ini:3: tecla desconhecida 'Zz' em [keyboard.1] cross");
    CHECK_THROWS_WITH(applyConfigText(o, "[gamepad.2]\nsquare = botaoinexistente\n", "anyps2.ini"),
                      "anyps2.ini:2: botão do controle desconhecido 'botaoinexistente' em [gamepad.2] square");
    CHECK_THROWS_WITH(applyConfigText(o, "[gamepad.1]\nlx = eixoinexistente\n", "anyps2.ini"),
                      "anyps2.ini:2: eixo do controle desconhecido 'eixoinexistente' em [gamepad.1] lx");
    // Nomes válidos do SDL passam.
    applyConfigText(o, "[keyboard.1]\ncross = Left Shift\n[gamepad.1]\ncross = a\n", "anyps2.ini");
}
#endif
