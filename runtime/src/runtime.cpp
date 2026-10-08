#include "anyps2/runtime/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "anyps2/common/bytes.h"
#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/generated.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/hardware.h"
#include "anyps2/runtime/iop.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/video.h"
#include "anyps2/runtime/vif.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace anyps2::rt {

RuntimeOptions RuntimeOptions::fromEnvironment() {
    RuntimeOptions o;
    if (const char* t = std::getenv("ANYPS2_TRACE")) {
        const std::string s(t);
        o.traceSyscalls = s.find("syscall") != std::string::npos || s == "all";
        o.traceCalls = s.find("call") != std::string::npos || s == "all";
        o.traceHardware = s.find("hw") != std::string::npos || s == "all";
        o.traceIop = s.find("iop") != std::string::npos || s == "all";
        o.traceGs = s.find("gs") != std::string::npos || s == "all";
    }
    if (const char* clock = std::getenv("ANYPS2_CLOCK")) o.virtualClock = std::string(clock) == "virtual";
    if (const char* v = std::getenv("ANYPS2_VIDEO")) o.video = v;
    if (const char* shot = std::getenv("ANYPS2_SCREENSHOT")) o.screenshot = shot;
    return o;
}

Runtime::Runtime(const ProgramInfo& program, RuntimeOptions options)
    : program_(program),
      options_(options),
      mem_(std::make_unique<Memory>()),
      ctx_(std::make_unique<Context>()) {
    std::memset(static_cast<void*>(ctx_.get()), 0, sizeof(Context));
    ctx_->mem = mem_.get();
    ctx_->rt = this;
    hw_ = std::make_unique<Hardware>(*this);
    iop_ = std::make_unique<Iop>(*this);
    timing_ = std::make_unique<Timing>(*this, options_.virtualClock ? Timing::Mode::Virtual : Timing::Mode::Real);
    kernel_ = std::make_unique<Kernel>(*this);
    vu_ = std::make_unique<VuMemory>();
    mem_->mapRam(0x11000000u, vu_->micro0.get(), VuMemory::kVu0Size);
    mem_->mapRam(0x11004000u, vu_->data0.get(), VuMemory::kVu0Size);
    mem_->mapRam(0x11008000u, vu_->micro1.get(), VuMemory::kVu1Size);
    mem_->mapRam(0x1100C000u, vu_->data1.get(), VuMemory::kVu1Size);
    gs_ = std::make_unique<gs::Gs>(this);
    gs_->setHsyncSource([](void* t) { return static_cast<Timing*>(t)->now(); }, timing_.get(),
                        Timing::kCyclesPerLine);
    gif_ = std::make_unique<Gif>(this, *gs_);
    vif0_ = std::make_unique<Vif>(this, 0, *vu_, gif_.get());
    vif1_ = std::make_unique<Vif>(this, 1, *vu_, gif_.get());
    dmac_ = std::make_unique<Dmac>(*this);
}

Runtime::~Runtime() = default;

void Runtime::onVblank() {
    if (!video_) return;
    if (gs_->displayEnabled()) video_->present(gs_->display());
    if (!video_->pollEvents()) throw ProgramExit{0};  // janela fechada
}

void Runtime::saveScreenshot() {
    if (options_.screenshot.empty()) return;
    const gs::Frame f = gs_->display();
    if (f.width == 0 || f.height == 0) {
        std::cerr << "anyps2: aviso: nenhuma imagem exibida (PMODE desligado); screenshot não gravado\n";
        return;
    }
    writePng(options_.screenshot, f);
}

void Runtime::loadImage(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw anyps2::Error("não foi possível abrir a imagem do programa '" + path + "'");
    std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    auto need = [&](std::size_t off, std::size_t n) {
        if (off + n > data.size()) throw anyps2::Error("imagem '" + path + "' truncada");
    };
    need(0, 20);
    if (std::memcmp(data.data(), "AP2IMG", 6) != 0) {
        throw anyps2::Error("'" + path + "' não é uma imagem do AnyPS2");
    }
    if (readLE32(data, 8) != 1) throw anyps2::Error("versão de imagem não suportada em '" + path + "'");
    const std::uint32_t count = readLE32(data, 16);
    std::size_t off = 20;
    for (std::uint32_t i = 0; i < count; ++i) {
        need(off, 12);
        const std::uint32_t vaddr = readLE32(data, off);
        const std::uint32_t filesz = readLE32(data, off + 4);
        const std::uint32_t memsz = readLE32(data, off + 8);
        off += 12;
        need(off, filesz);
        std::uint8_t* dst = mem_->hostPointer(vaddr, memsz);
        if (!dst) {
            throw anyps2::Error("segmento [" + anyps2::hex(vaddr) + ", +" + anyps2::hex(memsz) +
                                ") da imagem não cabe na RAM do EE");
        }
        std::memcpy(dst, data.data() + off, filesz);
        std::memset(dst + filesz, 0, memsz - filesz);
        off += filesz;
    }
}

const FunctionEntry* Runtime::lookup(std::uint32_t address) const {
    const FunctionEntry* begin = program_.functions;
    const FunctionEntry* end = begin + program_.functionCount;
    const FunctionEntry* it = std::upper_bound(
        begin, end, address, [](std::uint32_t a, const FunctionEntry& f) { return a < f.start; });
    if (it == begin) return nullptr;
    --it;
    return address < it->end ? it : nullptr;
}

std::string Runtime::describe(std::uint32_t address) const {
    if (const FunctionEntry* f = lookup(address)) {
        std::string s = f->name;
        if (address != f->start) s += "+" + anyps2::hex(address - f->start, 1);
        return s + " (" + anyps2::hex(address) + ")";
    }
    return anyps2::hex(address);
}

void Runtime::call(Context* c) {
    const FunctionEntry* f = lookup(c->pc);
    if (!f) {
        throw GuestError("salto/chamada para " + anyps2::hex(c->pc) +
                             ", que não pertence a nenhuma função recompilada (ra = " +
                             describe(c->r[31].uw[0]) + ")",
                         c->pc);
    }
    if (options_.traceCalls) std::fprintf(stderr, "[call] %s\n", describe(c->pc).c_str());
    f->fn(c);
}

void Runtime::runUntil(std::uint32_t stopPc) {
    Context* c = ctx_.get();
    while (c->pc != stopPc) call(c);
}

std::uint64_t Runtime::invokeGuest(std::uint32_t address, const std::vector<std::uint32_t>& args,
                                   std::uint32_t pc) {
    if (args.size() > 8) throw GuestError("invokeGuest: argumentos demais", pc);
    Context* c = ctx_.get();
    const Context saved = *c;
    for (std::size_t i = 0; i < args.size(); ++i) c->r[4 + i].sd[0] = static_cast<std::int32_t>(args[i]);
    c->r[31].sd[0] = static_cast<std::int32_t>(kHostReturn);
    // Pilha própria para código chamado pelo "kernel" (handlers, callbacks):
    // área de kernel do EE, que programas de usuário não usam.
    c->r[29].ud[0] = Kernel::kHandlerStackTop;
    c->pc = address;
    runUntil(kHostReturn);
    const std::uint64_t ret = c->r[2].ud[0];
    // Restaura os registradores do código interrompido; a memória fica.
    *c = saved;
    return ret;
}

void Runtime::syscall(Context* c, std::uint32_t pc) {
    kernel_->syscall(c, pc);
}

void Runtime::safepoint(Context* c, std::uint32_t pc, std::int64_t extraCycles) {
    // Instruções executadas desde a última recarga ~ ciclos (1 IPC).
    timing_->consume(static_cast<std::int64_t>(c->budgetReload) - c->budget + extraCycles);
    c->budget = c->budgetReload = 0;
    timing_->process(pc);
    kernel_->serviceInterrupts(pc);
    // Próximo safepoint: no máximo kChunk instruções, ou antes do próximo evento.
    constexpr std::uint64_t kChunk = 20000;
    const std::uint64_t untilEvent = timing_->cyclesUntilNextEvent();
    const auto chunk = static_cast<std::int32_t>(std::clamp<std::uint64_t>(untilEvent, 64, kChunk));
    c->budget = c->budgetReload = chunk;
}

int Runtime::run(const std::vector<std::string>& args, const std::string& imagePath) {
    loadImage(imagePath);
    Context* c = ctx_.get();
    // vf0 = (0, 0, 0, 1.0)
    c->vf[0].uw[3] = 0x3F800000u;
    c->fcr31 = 0x01000001u;
    c->cop0[cop0::PRId] = 0x00002E20u;   // R5900, revisão 2.0
    c->cop0[cop0::Config] = 0x00000440u;
    c->cop0[cop0::Status] = 0x70030C13u; // CU0-2, EIE, modo kernel
    kernel_->setBootArguments(args);
    c->budget = c->budgetReload = 20000;
    video_ = createVideo(options_, program_.name);
    const int code = kernel_->runMain(program_.entry);
    std::fflush(stdout);
    saveScreenshot();
    video_.reset();
    return code;
}

namespace gen {
void badEntry(Context* c, std::uint32_t functionStart) {
    throw GuestError("entrada em " + anyps2::hex(c->pc) + " no meio de " +
                         c->rt->describe(functionStart) +
                         ", que não é ponto de entrada conhecido (adicione-o na configuração)",
                     c->pc);
}
}  // namespace gen

int runProgram(const ProgramInfo& program, int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    namespace fs = std::filesystem;
    std::string image;
    if (const char* env = std::getenv("ANYPS2_IMAGE")) {
        image = env;
    } else {
        std::error_code ec;
        fs::path exe = argc > 0 ? fs::absolute(argv[0], ec) : fs::path();
        image = (exe.parent_path() / program.imageFile).string();
    }
    std::vector<std::string> args;
    args.push_back(std::string("host:") + program.name);
    for (int i = 1; i < argc; ++i) args.push_back(argv[i]);
    try {
        Runtime rt(program);
        return rt.run(args, image);
    } catch (const GuestError& e) {
        std::fflush(stdout);
        std::cerr << "anyps2: erro no guest: " << e.what() << "\n";
        return 3;
    } catch (const std::exception& e) {
        std::fflush(stdout);
        std::cerr << "anyps2: erro: " << e.what() << "\n";
        return 2;
    }
}

}  // namespace anyps2::rt
