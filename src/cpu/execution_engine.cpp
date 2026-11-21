#include "meridian/cpu/execution_engine.hpp"

namespace meridian::cpu {
namespace {

template <typename Memory>
ExecutionResult run_guest(const Interpreter& interpreter, CpuState& state,
                           Memory& memory,
                           std::uint64_t instruction_budget) noexcept {
    for (std::uint64_t step = 0; step < instruction_budget; ++step) {
        const StepResult result = interpreter.step(state, memory);
        if (result == StepResult::Halted)
            return {RunStopReason::Halted, step + 1U, result};
        if (result != StepResult::Executed)
            return {RunStopReason::Fault, step + 1U, result};
    }
    return {RunStopReason::InstructionLimit, instruction_budget,
            StepResult::Executed};
}

} // namespace

ExecutionResult ExecutionEngine::run(
    CpuState& state, core::GuestMemory& memory,
    std::uint64_t instruction_budget) const noexcept {
    return run_guest(interpreter_, state, memory, instruction_budget);
}

ExecutionResult ExecutionEngine::run(
    CpuState& state, core::VirtualAddressSpace& memory,
    std::uint64_t instruction_budget) const noexcept {
    return run_guest(interpreter_, state, memory, instruction_budget);
}

ExecutionResult ExecutionEngine::run(
    CpuState& state, core::X64PagedMemory& memory,
    std::uint64_t instruction_budget) const noexcept {
    return run_guest(interpreter_, state, memory, instruction_budget);
}

} // namespace meridian::cpu
