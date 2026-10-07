#include "anyps2/runtime/memory.h"

#include <algorithm>
#include <string>

#include "anyps2/runtime/errors.h"

namespace anyps2::rt {

namespace {
const char* sizeName(unsigned size) {
    switch (size) {
        case 1: return "8 bits";
        case 2: return "16 bits";
        case 4: return "32 bits";
        case 8: return "64 bits";
        default: return "128 bits";
    }
}
}  // namespace

Memory::Memory()
    : ram_(new std::uint8_t[kRamSize]()),
      spr_(new std::uint8_t[kScratchpadSize]()),
      pages_(new std::uint8_t*[std::size_t{1} << (32 - kPageBits)]()) {
    // RDRAM e seus espelhos.
    for (std::uint32_t mirror : {0x00000000u, 0x20000000u, 0x30000000u, 0x80000000u, 0xA0000000u}) {
        mapPages(mirror, ram_.get(), kRamSize);
    }
    mapPages(kScratchpadBase, spr_.get(), kScratchpadSize);
}

Memory::~Memory() = default;

void Memory::mapPages(std::uint32_t vbase, std::uint8_t* host, std::uint32_t size) {
    for (std::uint32_t off = 0; off < size; off += (1u << kPageBits)) {
        pages_[(vbase + off) >> kPageBits] = host + off;
    }
}

void Memory::mapDevice(std::uint32_t base, std::uint32_t size, MmioDevice* device) {
    for (const auto& d : devices_) {
        if (base < d.base + d.size && d.base < base + size) {
            throw anyps2::Error("faixas de MMIO sobrepostas em " + anyps2::hex(base));
        }
    }
    devices_.push_back({base, size, device});
    std::sort(devices_.begin(), devices_.end(),
              [](const DeviceRange& a, const DeviceRange& b) { return a.base < b.base; });
}

std::uint32_t Memory::physical(std::uint32_t addr) {
    if (addr >= 0x80000000u && addr < 0xC0000000u) return addr & 0x1FFFFFFFu;  // kseg0/kseg1
    if (addr >= 0x20000000u && addr < 0x40000000u) return addr & 0x0FFFFFFFu;  // uncached
    return addr;
}

MmioDevice* Memory::deviceFor(std::uint32_t addr, unsigned size) const {
    const std::uint32_t phys = physical(addr);
    for (const auto& d : devices_) {
        if (phys >= d.base && phys - d.base + size <= d.size) return d.device;
    }
    return nullptr;
}

std::uint8_t* Memory::hostPointer(std::uint32_t addr, std::uint32_t size) {
    std::uint8_t* first = pages_[addr >> kPageBits];
    if (!first) return nullptr;
    std::uint8_t* p = first + (addr & kPageMask);
    // Verifica que a faixa é contígua no host (RAM ou scratchpad).
    if (size > 0) {
        const std::uint32_t last = addr + size - 1;
        if (last < addr) return nullptr;
        std::uint8_t* lastPage = pages_[last >> kPageBits];
        if (!lastPage || lastPage + (last & kPageMask) != p + (size - 1)) return nullptr;
    }
    return p;
}

void Memory::slowRead(std::uint32_t addr, void* out, unsigned size, std::uint32_t pc) {
    if (pages_[addr >> kPageBits]) {
        throw GuestError("leitura desalinhada de " + std::string(sizeName(size)) + " em " +
                             anyps2::hex(addr) + " (exceção de endereço no hardware)",
                         pc);
    }
    if (MmioDevice* dev = deviceFor(addr, size)) {
        dev->read(physical(addr), out, size, pc);
        return;
    }
    throw GuestError("leitura de " + std::string(sizeName(size)) + " em endereço não mapeado " +
                         anyps2::hex(addr),
                     pc);
}

void Memory::slowWrite(std::uint32_t addr, const void* in, unsigned size, std::uint32_t pc) {
    if (pages_[addr >> kPageBits]) {
        throw GuestError("escrita desalinhada de " + std::string(sizeName(size)) + " em " +
                             anyps2::hex(addr) + " (exceção de endereço no hardware)",
                         pc);
    }
    if (MmioDevice* dev = deviceFor(addr, size)) {
        dev->write(physical(addr), in, size, pc);
        return;
    }
    throw GuestError("escrita de " + std::string(sizeName(size)) + " em endereço não mapeado " +
                         anyps2::hex(addr),
                     pc);
}

void Memory::copyFromGuest(void* dst, std::uint32_t addr, std::uint32_t size, std::uint32_t pc) {
    if (size == 0) return;
    if (std::uint8_t* p = hostPointer(addr, size)) {
        std::memcpy(dst, p, size);
        return;
    }
    throw GuestError("faixa [" + anyps2::hex(addr) + ", +" + anyps2::hex(size) +
                         ") não está em RAM/scratchpad",
                     pc);
}

void Memory::copyToGuest(std::uint32_t addr, const void* src, std::uint32_t size, std::uint32_t pc) {
    if (size == 0) return;
    if (std::uint8_t* p = hostPointer(addr, size)) {
        std::memcpy(p, src, size);
        return;
    }
    throw GuestError("faixa [" + anyps2::hex(addr) + ", +" + anyps2::hex(size) +
                         ") não está em RAM/scratchpad",
                     pc);
}

std::string Memory::readCString(std::uint32_t addr, std::uint32_t maxLen, std::uint32_t pc) {
    std::string s;
    for (std::uint32_t i = 0; i < maxLen; ++i) {
        std::uint8_t* p = hostPointer(addr + i, 1);
        if (!p) throw GuestError("string em " + anyps2::hex(addr) + " sai da RAM", pc);
        if (*p == 0) return s;
        s.push_back(static_cast<char>(*p));
    }
    throw GuestError("string em " + anyps2::hex(addr) + " sem terminador em " +
                         std::to_string(maxLen) + " bytes",
                     pc);
}

}  // namespace anyps2::rt
