#include "meridian/cpu/interpreter.hpp"

#include <cstddef>
#include <cstdint>

namespace meridian::cpu {
namespace {
bool fetch8(const core::GuestMemory& memory, std::uint64_t& pc, std::uint8_t& out) noexcept {
    const auto value = memory.read_le<std::uint8_t>(pc);
    if (!value) return false;
    out = *value;
    ++pc;
    return true;
}
} // namespace

StepResult Interpreter::step(CpuState& state, const core::GuestMemory& memory) const noexcept {
    std::uint64_t pc = state.rip;
    std::uint8_t opcode = 0;
    if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;

    if (opcode == 0x90) { // NOP
        state.rip = pc;
        return StepResult::Executed;
    }
    if (opcode == 0xf4) { // HLT
        state.rip = pc;
        return StepResult::Halted;
    }

    // MOV r64, imm64 / MOV r32, imm32. REX.B selects R8-R15.
    bool rex_w = false;
    bool rex_b = false;
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex_w = (opcode & 0x08U) != 0;
        rex_b = (opcode & 0x01U) != 0;
        if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;
    }
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        const std::size_t reg = static_cast<std::size_t>(opcode - 0xb8) + (rex_b ? 8U : 0U);
        if (rex_w) {
            const auto immediate = memory.read_le<std::uint64_t>(pc);
            if (!immediate) return StepResult::MemoryFault;
            state.gpr[reg] = *immediate;
            pc += 8;
        } else {
            const auto immediate = memory.read_le<std::uint32_t>(pc);
            if (!immediate) return StepResult::MemoryFault;
            state.gpr[reg] = *immediate; // 32-bit writes zero-extend.
            pc += 4;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    return StepResult::UnsupportedInstruction;
}

} // namespace meridian::cpu
