#include "minitest.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace minitest {

namespace {
int g_failures = 0;
const TestCase* g_current = nullptr;
}  // namespace

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

void reportFailure(const char* file, int line, const std::string& message) {
    ++g_failures;
    std::cout << "  FALHA [" << (g_current ? g_current->suite + "." + g_current->name : "?")
              << "] " << file << ":" << line << "\n      " << message << "\n";
}

}  // namespace minitest

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // Os testes unitários foram escritos para o horizonte W = 0 (as respostas do GS valem na hora,
    // como antes da fatia 6 do plano do VU1 em outra thread). Os testes do horizonte trocam W com
    // Vu1Stream::setHorizon; quem quiser outro W para a suíte inteira define ANYPS2_VU1_HORIZON.
    if (!std::getenv("ANYPS2_VU1_HORIZON")) {
#ifdef _WIN32
        _putenv_s("ANYPS2_VU1_HORIZON", "0");
#else
        setenv("ANYPS2_VU1_HORIZON", "0", 1);
#endif
    }
    using namespace minitest;
    std::string filter;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--list") list = true;
        else filter = a;
    }

    int run = 0, failedTests = 0;
    for (const auto& t : registry()) {
        const std::string full = t.suite + "." + t.name;
        if (!filter.empty() && t.suite != filter && full != filter) continue;
        if (list) {
            std::cout << full << "\n";
            continue;
        }
        ++run;
        g_current = &t;
        const int before = g_failures;
        try {
            t.fn();
        } catch (const AbortTest&) {
        } catch (const std::exception& e) {
            reportFailure("?", 0, std::string("exceção não tratada: ") + e.what());
        } catch (...) {
            reportFailure("?", 0, "exceção desconhecida");
        }
        const bool ok = g_failures == before;
        if (!ok) ++failedTests;
        std::cout << (ok ? "[ ok ] " : "[FALHOU] ") << full << "\n";
    }
    if (list) return 0;
    if (run == 0) {
        std::cout << "Nenhum teste corresponde ao filtro '" << filter << "'\n";
        return 1;
    }
    std::cout << "\n" << run << " teste(s), " << failedTests << " falharam, " << g_failures
              << " verificação(ões) com falha\n";
    return failedTests == 0 ? 0 : 1;
}
