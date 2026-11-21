#pragma once

#include "meridian/core/guest_memory.hpp"
#include "meridian/core/virtual_address_space.hpp"
#include "meridian/cpu/cpu_state.hpp"

#include <cstdint>

namespace meridian::cpu {

enum class StepResult {
    Executed,
    Halted,
    MemoryFault,
    UnsupportedInstruction
};

// Small, deliberately incomplete x86-64 interpreter used to grow and test
// the CPU backend. It is not a complete x86-64 implementation.
class Interpreter {
public:
    [[nodiscard]] StepResult step(CpuState& state, core::GuestMemory& memory) const noexcept;
    [[nodiscard]] StepResult step(CpuState& state, core::VirtualAddressSpace& memory) const noexcept;
};

} // namespace meridian::cpu
