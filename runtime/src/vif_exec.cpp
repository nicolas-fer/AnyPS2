#include "anyps2/runtime/vif_exec.h"

#include <cstring>

#include "anyps2/runtime/gif.h"
#include "anyps2/runtime/host_profile.h"
#include "anyps2/runtime/runtime.h"
#include "anyps2/runtime/vif.h"
#include "anyps2/runtime/vu/vu.h"

namespace anyps2::rt {

VifExec::VifExec(Runtime* rt, unsigned unit, VuMemory& vu, Gif* gif) : rt_(rt), unit_(unit), vu_(vu), gif_(gif) {}

void VifExec::apply(const VifItem& item) {
    switch (item.kind) {
        case VifItem::Kind::Unpack: {
            std::uint8_t* mem = unit_ == 0 ? vu_.data0.get() : vu_.data1.get();
            for (std::size_t i = 0; i < item.count; ++i) {
                const VifQwWrite& w = item.writes[i];
                std::uint8_t* dst = mem + std::size_t{w.qw} * 16;
                for (unsigned c = 0; c < 4; ++c) {
                    if (w.mask & (1u << c)) std::memcpy(dst + c * 4, &w.v[c], 4);
                }
            }
            break;
        }
        case VifItem::Kind::Mpg: {
            std::uint8_t* mem = unit_ == 0 ? vu_.micro0.get() : vu_.micro1.get();
            const std::uint32_t size = unit_ == 0 ? VuMemory::kVu0Size : VuMemory::kVu1Size;
            std::uint32_t addr = item.addr;
            for (std::size_t i = 0; i < item.count; ++i, addr += 4) {
                std::memcpy(mem + (addr & (size - 4)), &item.words[i], 4);
            }
            break;
        }
        case VifItem::Kind::Mscal:
        case VifItem::Kind::Mscnt: {
            const bool vif1 = unit_ == 1;
            if (vif1) HostProfile::noteMscal(item.kind == VifItem::Kind::Mscnt);
            Vu& vu = vif1 ? rt_->vu1() : rt_->vu0();
            const std::uint32_t start =
                item.kind == VifItem::Kind::Mscnt ? vu.regs().vi[vucore::reg::TPC] * 8 : item.start;
            if (vif1) {
                vu.startVif(start, item.pc, item.top, item.itop, item.vblanks);
            } else {
                vu.start(start, item.pc);
            }
            break;
        }
        case VifItem::Kind::Direct:
            gif_->transfer(2, item.data, 1, item.pc);
            break;
        case VifItem::Kind::Path3Mask:
            gif_->setPath3Masked(item.masked);
            break;
        case VifItem::Kind::Reset:
            if (unit_ == 1) {
                if (gif_) gif_->setPath3Masked(false);
                if (rt_) rt_->vu1().setVifTop(0, 0);
            }
            break;
    }
}

}  // namespace anyps2::rt
