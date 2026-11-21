#include "meridian/cpu/cpu_state.hpp"

namespace meridian::cpu {

void CpuState::reset() noexcept {
    gpr.fill(0);
    rip = 0;
    rflags = 0x2;
    for (auto& reg : xmm) reg.fill(std::byte{0});
}

} // namespace meridian::cpu
