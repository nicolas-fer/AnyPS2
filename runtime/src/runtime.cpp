#include "anyps2/runtime/runtime.h"

#include <algorithm>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#include "anyps2/common/bytes.h"
#include "anyps2/runtime/audio.h"
#include "anyps2/runtime/config.h"
#include "anyps2/runtime/dmac.h"
#include "anyps2/runtime/errors.h"
#include "anyps2/runtime/generated.h"
#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/gs/gs.h"
#include "anyps2/runtime/hardware.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/input.h"
#include "anyps2/runtime/ipu.h"
#include "anyps2/runtime/iop/iop.h"
#include "anyps2/runtime/kernel.h"
#include "anyps2/runtime/timing.h"
#include "anyps2/runtime/video.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"
#include "anyps2/runtime/vu/vu_trace.h"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace anyps2::rt {

RuntimeOptions RuntimeOptions::fromEnvironment() {
    return resolveOptions([](const char* name) -> const char* { return std::getenv(name); }, defaultConfigPath());
}

Runtime::Runtime(const ProgramInfo& program, RuntimeOptions options)
    : program_(program),
      options_(options),
      mem_(std::make_unique<Memory>()),
      ctx_(std::make_unique<Context>()) {
    std::memset(static_cast<void*>(ctx_.get()), 0, sizeof(Context));
    ctx_->mem = mem_.get();
    ctx_->rt = this;
    if (options_.profile) HostProfile::enable();
    input_ = std::make_unique<Input>();
    if (!options_.padScript.empty()) input_->loadScript(options_.padScript);
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
    gs_->setRealTimeClock(!options_.virtualClock);
    gif_ = std::make_unique<Gif>(this, *gs_);
    vif0_ = std::make_unique<Vif>(this, 0, *vu_, gif_.get());
    vif1_ = std::make_unique<Vif>(this, 1, *vu_, gif_.get());
    dmac_ = std::make_unique<Dmac>(*this);
    ipu_ = std::make_unique<Ipu>(this);
    vu1Regs_ = std::make_unique<Vu1Regs>();
    std::memset(static_cast<void*>(vu1Regs_.get()), 0, sizeof(Vu1Regs));
    vu0_ = std::make_unique<Vu>(this, 0, vucore::Regs{ctx_->vf, ctx_->vi, &ctx_->vacc}, vu_->data0.get(),
                                VuMemory::kVu0Size, vu_->micro0.get(), VuMemory::kVu0Size);
    vu1_ = std::make_unique<Vu>(this, 1, vucore::Regs{vu1Regs_->vf, vu1Regs_->vi, &vu1Regs_->acc},
                                vu_->data1.get(), VuMemory::kVu1Size, vu_->micro1.get(), VuMemory::kVu1Size);
    for (Vu* v : {vu0_.get(), vu1_.get()}) {
        v->setPrograms(program.vuPrograms, program.vuProgramCount);
        if (options_.vuMode == "interp") v->setMode(Vu::Mode::Interpret);
        else if (options_.vuMode == "compiled") v->setMode(Vu::Mode::CompiledOnly);
        else if (!options_.vuMode.empty() && options_.vuMode != "auto") {
            throw anyps2::Error("ANYPS2_VU=" + options_.vuMode + " desconhecido (use interp, compiled ou auto)");
        }
        v->setDumpDir(options_.vuDumpDir);
    }
    vuTrace_ = VuTrace::fromEnv();
    if (vuTrace_) vu1_->setTrace(vuTrace_.get());
}

Runtime::~Runtime() {
    printProfile();
    if (options_.profile) std::fputs(HostProfile::report().c_str(), stderr);
}

void Runtime::onVblank(std::uint32_t pc) {
    ++vblanks_;
    iop_->vblank(pc);
    if (options_.screenshotEvery && !options_.screenshot.empty() && vblanks_ % options_.screenshotEvery == 0 &&
        gs_->displayEnabled()) {
        std::string base = options_.screenshot;
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".png") == 0) base.resize(base.size() - 4);
        char suffix[32];
        std::snprintf(suffix, sizeof suffix, "_%06llu.png", static_cast<unsigned long long>(vblanks_));
        writePng(base + suffix, gs_->display());
    }
    if (options_.profile && vblanks_ % 500 == 0) {
        std::fputs(HostProfile::interval(vblanks_).c_str(), stderr);
        // Pares de VU desde o começo: compilados × interpretados (microcódigo
        // que só existe em tempo de execução cai no interpretador).
        std::fprintf(stderr, "[perfil] pares de VU: VU0 %llu compilados / %llu interpretados; VU1 %llu / %llu\n",
                     static_cast<unsigned long long>(vu0_->compiledPairs()),
                     static_cast<unsigned long long>(vu0_->interpretedPairs()),
                     static_cast<unsigned long long>(vu1_->compiledPairs()),
                     static_cast<unsigned long long>(vu1_->interpretedPairs()));
    }
    if (options_.frames && vblanks_ >= options_.frames) {
        if (options_.traceThreads) {
            std::fprintf(stderr, "[threads] no VBlank %llu:%s\n", static_cast<unsigned long long>(vblanks_),
                         kernel_->threadReport().c_str());
        }
        throw ProgramExit{0};
    }
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
        if (const auto stub = Kernel::syscallStub(c->pc)) {
            kernel_->callSyscallStub(c, *stub, c->pc);
            return;
        }
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

void Runtime::printProfile() const {
    if (profile_.empty()) return;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> top;
    std::uint64_t total = 0;
    for (const auto& [pc, n] : profile_) {
        top.emplace_back(n, pc);
        total += n;
    }
    std::sort(top.begin(), top.end(), std::greater<>());
    std::fprintf(stderr, "[perfil] %llu amostras; mais frequentes:\n", static_cast<unsigned long long>(total));
    for (std::size_t i = 0; i < top.size() && i < 20; ++i) {
        std::fprintf(stderr, "[perfil] %6.2f%%  %s\n", 100.0 * static_cast<double>(top[i].first) / static_cast<double>(total),
                     describe(top[i].second).c_str());
    }
}

void Runtime::safepoint(Context* c, std::uint32_t pc, std::int64_t extraCycles) {
    if (options_.profile) ++profile_[pc];
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
    kernel_->initSyscallTable();
    Context* c = ctx_.get();
    // vf0 = (0, 0, 0, 1.0)
    c->vf[0].uw[3] = 0x3F800000u;
    c->fcr31 = 0x01000001u;
    c->cop0[cop0::PRId] = 0x00002E20u;   // R5900, revisão 2.0
    c->cop0[cop0::Config] = 0x00000440u;
    c->cop0[cop0::Status] = 0x70030C13u; // CU0-2, EIE, modo kernel
    kernel_->setBootArguments(args);
    c->budget = c->budgetReload = 20000;
    audio_ = std::make_unique<Audio>(options_);
    video_ = createVideo(options_, program_.name, input_.get());
    const int code = kernel_->runMain(program_.entry);
    std::fflush(stdout);
    iop_->flushAudio(0);
    saveScreenshot();
    video_.reset();
    audio_.reset();
    return code;
}

namespace gen {
void rtCall(Context* c) {
    c->rt->call(c);
}
void rtSyscall(Context* c, std::uint32_t pc) {
    c->rt->syscall(c, pc);
}
void rtSafepoint(Context* c, std::uint32_t pc) {
    c->rt->safepoint(c, pc);
}

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
