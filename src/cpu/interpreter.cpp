#include "meridian/cpu/interpreter.hpp"

#include <bit>
#include <cstddef>
#include <cstdint>

namespace meridian::cpu {
namespace {
constexpr std::uint64_t flag_cf = 1ULL << 0;
constexpr std::uint64_t flag_pf = 1ULL << 2;
constexpr std::uint64_t flag_af = 1ULL << 4;
constexpr std::uint64_t flag_zf = 1ULL << 6;
constexpr std::uint64_t flag_sf = 1ULL << 7;
constexpr std::uint64_t flag_of = 1ULL << 11;
constexpr std::uint64_t arithmetic_flags = flag_cf | flag_pf | flag_af | flag_zf | flag_sf | flag_of;

bool fetch8(const core::GuestMemory& memory, std::uint64_t& pc, std::uint8_t& out) noexcept {
    const auto value = memory.read_le<std::uint8_t>(pc);
    if (!value) return false;
    out = *value;
    ++pc;
    return true;
}

void update_common_flags(CpuState& state, std::uint64_t result) noexcept {
    if (result == 0) state.rflags |= flag_zf;
    if ((result >> 63U) != 0) state.rflags |= flag_sf;
    if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0) state.rflags |= flag_pf;
}

void update_add_flags(CpuState& state, std::uint64_t lhs, std::uint64_t rhs,
                      std::uint64_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (result < lhs) state.rflags |= flag_cf;
    if (((~(lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0) state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags(state, result);
}

void update_sub_flags(CpuState& state, std::uint64_t lhs, std::uint64_t rhs,
                      std::uint64_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (lhs < rhs) state.rflags |= flag_cf;
    if ((((lhs ^ rhs) & (lhs ^ result)) >> 63U) != 0) state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags(state, result);
}

void update_logic_flags(CpuState& state, std::uint64_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    update_common_flags(state, result);
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

    // Short relative branches: JMP, JZ, and JNZ.
    if (opcode == 0xeb || opcode == 0x74 || opcode == 0x75) {
        std::uint8_t raw_displacement = 0;
        if (!fetch8(memory, pc, raw_displacement)) return StepResult::MemoryFault;
        const auto displacement = static_cast<std::int8_t>(raw_displacement);
        const bool zero = (state.rflags & flag_zf) != 0;
        const bool take = opcode == 0xeb || (opcode == 0x74 && zero) ||
                          (opcode == 0x75 && !zero);
        state.rip = take ? pc + static_cast<std::int64_t>(displacement) : pc;
        return StepResult::Executed;
    }

    // Near relative JZ/JNZ (0F 84 / 0F 85), with a signed 32-bit offset.
    if (opcode == 0x0f) {
        std::uint8_t second = 0;
        if (!fetch8(memory, pc, second)) return StepResult::MemoryFault;
        if (second != 0x84 && second != 0x85) return StepResult::UnsupportedInstruction;
        const auto raw_displacement = memory.read_le<std::uint32_t>(pc);
        if (!raw_displacement) return StepResult::MemoryFault;
        pc += 4;
        const auto displacement = static_cast<std::int32_t>(*raw_displacement);
        const bool zero = (state.rflags & flag_zf) != 0;
        const bool take = second == 0x84 ? zero : !zero;
        state.rip = take ? pc + static_cast<std::int64_t>(displacement) : pc;
        return StepResult::Executed;
    }

    bool rex_w = false;
    bool rex_r = false;
    bool rex_b = false;
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex_w = (opcode & 0x08U) != 0;
        rex_r = (opcode & 0x04U) != 0;
        rex_b = (opcode & 0x01U) != 0;
        if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;
    }

    // MOV r64, imm64 / MOV r32, imm32. REX.B selects R8-R15.
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
            state.gpr[reg] = *immediate;
            pc += 4;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // Register-only 64-bit ADD, SUB, XOR, and CMP: r/m64, r64.
    if (rex_w && (opcode == 0x01 || opcode == 0x29 || opcode == 0x31 || opcode == 0x39)) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if ((modrm & 0xc0U) != 0xc0U) return StepResult::UnsupportedInstruction;

        const std::size_t source = static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
        const std::size_t destination = static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
        const std::uint64_t lhs = state.gpr[destination];
        const std::uint64_t rhs = state.gpr[source];
        std::uint64_t result = 0;

        switch (opcode) {
        case 0x01: // ADD
            result = lhs + rhs;
            update_add_flags(state, lhs, rhs, result);
            state.gpr[destination] = result;
            break;
        case 0x29: // SUB
            result = lhs - rhs;
            update_sub_flags(state, lhs, rhs, result);
            state.gpr[destination] = result;
            break;
        case 0x31: // XOR
            result = lhs ^ rhs;
            update_logic_flags(state, result);
            state.gpr[destination] = result;
            break;
        case 0x39: // CMP (subtract for flags, discard result)
            result = lhs - rhs;
            update_sub_flags(state, lhs, rhs, result);
            break;
        default:
            return StepResult::UnsupportedInstruction;
        }

        state.rip = pc;
        return StepResult::Executed;
    }

    return StepResult::UnsupportedInstruction;
}

} // namespace meridian::cpu
