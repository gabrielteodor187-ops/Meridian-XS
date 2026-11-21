#pragma once

#include "meridian/core/guest_memory.hpp"
#include "meridian/core/virtual_address_space.hpp"
#include "meridian/core/x64_paged_memory.hpp"
#include "meridian/cpu/interpreter.hpp"

#include <cstdint>

namespace meridian::cpu {

enum class RunStopReason {
    Halted,
    InstructionLimit,
    Fault
};

struct ExecutionResult {
    RunStopReason reason{RunStopReason::InstructionLimit};
    std::uint64_t steps{0};
    StepResult last_step{StepResult::Executed};
};

// Bounded guest execution loop. A hard instruction budget prevents a guest
// from monopolizing the host when it loops or never reaches HLT.
class ExecutionEngine {
public:
    [[nodiscard]] ExecutionResult run(CpuState& state,
                                      core::GuestMemory& memory,
                                      std::uint64_t instruction_budget) const noexcept;
    [[nodiscard]] ExecutionResult run(CpuState& state,
                                      core::VirtualAddressSpace& memory,
                                      std::uint64_t instruction_budget) const noexcept;
    [[nodiscard]] ExecutionResult run(CpuState& state,
                                      core::X64PagedMemory& memory,
                                      std::uint64_t instruction_budget) const noexcept;

private:
    Interpreter interpreter_{};
};

} // namespace meridian::cpu
