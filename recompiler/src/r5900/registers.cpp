#include "anyps2/r5900/registers.h"

#include <array>

namespace anyps2::r5900 {

std::string_view gprName(std::uint32_t index) {
    static constexpr std::array<std::string_view, 32> kNames = {
        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
        "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
        "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra"};
    return kNames[index & 31];
}

std::string_view cop0Name(std::uint32_t index) {
    static constexpr std::array<std::string_view, 32> kNames = {
        "c0_index",    "c0_random", "c0_entrylo0", "c0_entrylo1", "c0_context", "c0_pagemask",
        "c0_wired",    "",          "c0_badvaddr", "c0_count",    "c0_entryhi", "c0_compare",
        "c0_sr",       "c0_cause",  "c0_epc",      "c0_prid",     "c0_config",  "",
        "",            "",          "",            "",            "",           "c0_badpaddr",
        "",            "",          "",            "",            "c0_taglo",   "c0_taghi",
        "c0_errorepc", ""};
    return kNames[index & 31];
}

}  // namespace anyps2::r5900
