#pragma once

// Mini framework de testes do AnyPS2 (sem dependências externas).
//
//   TEST_CASE(suite, nome) { CHECK(...); CHECK_EQ(a, b); REQUIRE(...); }
//
// Executável: anyps2_tests [suite[.nome]] [--list]

#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace minitest {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry();

struct Registrar {
    Registrar(const char* suite, const char* name, void (*fn)()) {
        registry().push_back({suite, name, fn});
    }
};

// Interrompe o teste atual (REQUIRE).
struct AbortTest {};

void reportFailure(const char* file, int line, const std::string& message);

template <typename T>
std::string show(const T& v) {
    std::ostringstream os;
    if constexpr (std::is_same_v<T, bool>) {
        os << (v ? "true" : "false");
    } else if constexpr (std::is_enum_v<T>) {
        os << static_cast<long long>(static_cast<std::underlying_type_t<T>>(v));
    } else if constexpr (std::is_integral_v<T> && !std::is_same_v<T, char>) {
        os << +v;
        if (v < 0 || v > 9) os << " (0x" << std::hex << +v << ")";
    } else if constexpr (std::is_convertible_v<T, std::string_view>) {
        os << '"' << std::string_view(v) << '"';
    } else {
        os << v;
    }
    return os.str();
}

}  // namespace minitest

#define MT_CONCAT2(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT2(a, b)

#define TEST_CASE(suite, name)                                                        \
    static void MT_CONCAT(mt_test_, MT_CONCAT(suite, MT_CONCAT(_, name)))();          \
    static ::minitest::Registrar MT_CONCAT(mt_reg_, MT_CONCAT(suite, MT_CONCAT(_, name)))( \
        #suite, #name, &MT_CONCAT(mt_test_, MT_CONCAT(suite, MT_CONCAT(_, name))));   \
    static void MT_CONCAT(mt_test_, MT_CONCAT(suite, MT_CONCAT(_, name)))()

#define CHECK(expr)                                                                   \
    do {                                                                              \
        if (!(expr)) ::minitest::reportFailure(__FILE__, __LINE__, "CHECK(" #expr ")"); \
    } while (0)

#define REQUIRE(expr)                                                                 \
    do {                                                                              \
        if (!(expr)) {                                                                \
            ::minitest::reportFailure(__FILE__, __LINE__, "REQUIRE(" #expr ")");     \
            throw ::minitest::AbortTest{};                                            \
        }                                                                             \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        const auto& mt_a = (a);                                                       \
        const auto& mt_b = (b);                                                       \
        if (!(mt_a == mt_b)) {                                                        \
            ::minitest::reportFailure(__FILE__, __LINE__,                             \
                                      "CHECK_EQ(" #a ", " #b ")\n      obtido:   " +  \
                                          ::minitest::show(mt_a) +                    \
                                          "\n      esperado: " + ::minitest::show(mt_b)); \
        }                                                                             \
    } while (0)

#define CHECK_MSG(expr, msg)                                                          \
    do {                                                                              \
        if (!(expr))                                                                  \
            ::minitest::reportFailure(__FILE__, __LINE__,                             \
                                      std::string("CHECK(" #expr "): ") + (msg));     \
    } while (0)

// Verifica que a expressão lança uma exceção derivada de std::exception
// cuja mensagem contém o trecho indicado.
#define CHECK_THROWS_WITH(expr, substring)                                            \
    do {                                                                              \
        bool mt_threw = false;                                                        \
        try {                                                                         \
            (void)(expr);                                                             \
        } catch (const std::exception& mt_e) {                                        \
            mt_threw = true;                                                          \
            if (std::string_view(mt_e.what()).find(substring) == std::string_view::npos) \
                ::minitest::reportFailure(__FILE__, __LINE__,                         \
                                          std::string("mensagem de erro inesperada: \"") + \
                                              mt_e.what() + "\" (esperado conter \"" + \
                                              (substring) + "\")");                   \
        }                                                                             \
        if (!mt_threw)                                                                \
            ::minitest::reportFailure(__FILE__, __LINE__,                             \
                                      "esperava exceção em " #expr);                  \
    } while (0)
