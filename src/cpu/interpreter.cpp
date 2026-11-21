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
constexpr std::size_t rsp_index = 4;

template <typename Memory>
bool fetch8(const Memory& memory, std::uint64_t& pc, std::uint8_t& out) noexcept {
    const auto value = memory.template fetch_le<std::uint8_t>(pc);
    if (!value) return false;
    out = *value;
    ++pc;
    return true;
}

template <typename Memory>
bool decode_effective_address(const CpuState& state, const Memory& memory,
                              std::uint64_t& pc, std::uint8_t modrm,
                              bool rex_b, bool rex_x, std::size_t trailing_bytes,
                              std::uint64_t& address) noexcept {
    const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
    if (mod == 3) return false;

    const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
    std::uint64_t base_value = 0;
    std::uint64_t index_value = 0;
    unsigned scale_shift = 0;
    bool has_base = true;
    bool rip_relative = false;

    if (rm_low == 4) {
        std::uint8_t sib = 0;
        if (!fetch8(memory, pc, sib)) return false;
        scale_shift = static_cast<unsigned>(sib >> 6U);
        const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
        const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
        if (index_low != 4 || rex_x) {
            const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                          (rex_x ? 8U : 0U);
            index_value = state.gpr[index_reg];
        }
        if (mod == 0 && base_low == 5) {
            has_base = false;
        } else {
            const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                         (rex_b ? 8U : 0U);
            base_value = state.gpr[base_reg];
        }
    } else if (mod == 0 && rm_low == 5) {
        rip_relative = true;
    } else {
        const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                     (rex_b ? 8U : 0U);
        base_value = state.gpr[base_reg];
    }

    std::int64_t displacement = 0;
    if (mod == 1) {
        std::uint8_t raw = 0;
        if (!fetch8(memory, pc, raw)) return false;
        displacement = std::bit_cast<std::int8_t>(raw);
    } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
        const auto raw = memory.template fetch_le<std::uint32_t>(pc);
        if (!raw) return false;
        displacement = std::bit_cast<std::int32_t>(*raw);
        pc += 4;
    }

    address = (rip_relative ? pc + trailing_bytes : (has_base ? base_value : 0)) +
              (index_value << scale_shift) +
              static_cast<std::uint64_t>(displacement);
    return true;
}

template <typename Memory>
bool push64(CpuState& state, Memory& memory, std::uint64_t value) noexcept {
    const std::uint64_t old_rsp = state.gpr[rsp_index];
    if (old_rsp < sizeof(std::uint64_t)) return false;
    const std::uint64_t new_rsp = old_rsp - sizeof(std::uint64_t);
    if (!memory.template write_le<std::uint64_t>(new_rsp, value)) return false;
    state.gpr[rsp_index] = new_rsp;
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

void update_common_flags32(CpuState& state, std::uint32_t result) noexcept {
    if (result == 0) state.rflags |= flag_zf;
    if ((result & 0x80000000U) != 0) state.rflags |= flag_sf;
    if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0)
        state.rflags |= flag_pf;
}

void update_add_flags32(CpuState& state, std::uint32_t lhs, std::uint32_t rhs,
                        std::uint32_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (result < lhs) state.rflags |= flag_cf;
    if (((~(lhs ^ rhs) & (lhs ^ result)) & 0x80000000U) != 0)
        state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags32(state, result);
}

void update_sub_flags32(CpuState& state, std::uint32_t lhs, std::uint32_t rhs,
                        std::uint32_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (lhs < rhs) state.rflags |= flag_cf;
    if ((((lhs ^ rhs) & (lhs ^ result)) & 0x80000000U) != 0)
        state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags32(state, result);
}

void update_logic_flags32(CpuState& state, std::uint32_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    update_common_flags32(state, result);
}


bool canonical_address(std::uint64_t address) noexcept {
    const std::uint64_t upper = address >> 48U;
    const bool sign = ((address >> 47U) & 1U) != 0;
    return sign ? upper == 0xffffU : upper == 0;
}

bool read_msr(const CpuState& state, std::uint32_t index,
              std::uint64_t& value) noexcept {
    switch (index) {
    case 0x00000010U: value = state.tsc; return true;
    case 0x0000001bU: value = state.apic_base; return true;
    case 0x00000174U: value = state.sysenter_cs; return true;
    case 0x00000175U: value = state.sysenter_esp; return true;
    case 0x00000176U: value = state.sysenter_eip; return true;
    case 0xc0000080U: value = state.efer; return true;
    case 0xc0000081U: value = state.star; return true;
    case 0xc0000082U: value = state.lstar; return true;
    case 0xc0000083U: value = state.cstar; return true;
    case 0xc0000084U: value = state.sfmask; return true;
    case 0xc0000100U: value = state.fs_base; return true;
    case 0xc0000101U: value = state.gs_base; return true;
    case 0xc0000102U: value = state.kernel_gs_base; return true;
    case 0xc0000103U: value = state.tsc_aux; return true;
    default: return false;
    }
}

bool write_msr(CpuState& state, std::uint32_t index,
               std::uint64_t value) noexcept {
    switch (index) {
    case 0x00000010U: state.tsc = value; return true;
    case 0x0000001bU: state.apic_base = value; return true;
    case 0x00000174U: state.sysenter_cs = value; return true;
    case 0x00000175U: state.sysenter_esp = value; return true;
    case 0x00000176U: state.sysenter_eip = value; return true;
    case 0xc0000080U: state.efer = value; return true;
    case 0xc0000081U: state.star = value; return true;
    case 0xc0000082U: state.lstar = value; return true;
    case 0xc0000083U: state.cstar = value; return true;
    case 0xc0000084U: state.sfmask = value; return true;
    case 0xc0000100U: state.fs_base = value; return true;
    case 0xc0000101U: state.gs_base = value; return true;
    case 0xc0000102U: state.kernel_gs_base = value; return true;
    case 0xc0000103U: state.tsc_aux = value; return true;
    default: return false;
    }
}

bool condition_true(std::uint8_t condition, std::uint64_t flags) noexcept {
    const bool carry = (flags & flag_cf) != 0;
    const bool parity = (flags & flag_pf) != 0;
    const bool zero = (flags & flag_zf) != 0;
    const bool sign = (flags & flag_sf) != 0;
    const bool overflow = (flags & flag_of) != 0;
    switch (condition & 0x0fU) {
    case 0x0: return overflow;                 // O
    case 0x1: return !overflow;                // NO
    case 0x2: return carry;                    // B/ C
    case 0x3: return !carry;                   // AE/ NC
    case 0x4: return zero;                     // E/ Z
    case 0x5: return !zero;                    // NE/ NZ
    case 0x6: return carry || zero;             // BE
    case 0x7: return !carry && !zero;           // A
    case 0x8: return sign;                     // S
    case 0x9: return !sign;                    // NS
    case 0xa: return parity;                   // P/ PE
    case 0xb: return !parity;                  // NP/ PO
    case 0xc: return sign != overflow;          // L
    case 0xd: return sign == overflow;          // GE
    case 0xe: return zero || (sign != overflow);// LE
    case 0xf: return !zero && (sign == overflow);// G
    }
    return false;
}

} // namespace

template <typename Memory>
StepResult step_impl(CpuState& state, Memory& memory) noexcept {
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

    // Near relative CALL rel32 and RET. The stack grows toward lower addresses.
    if (opcode == 0xe8) {
        const auto raw_displacement = memory.template fetch_le<std::uint32_t>(pc);
        if (!raw_displacement) return StepResult::MemoryFault;
        pc += 4;
        const auto displacement = std::bit_cast<std::int32_t>(*raw_displacement);
        if (!push64(state, memory, pc)) return StepResult::MemoryFault;
        state.rip = pc + static_cast<std::int64_t>(displacement);
        return StepResult::Executed;
    }
    if (opcode == 0xc3) {
        const std::uint64_t old_rsp = state.gpr[rsp_index];
        if (old_rsp > UINT64_MAX - sizeof(std::uint64_t)) return StepResult::MemoryFault;
        const auto target = memory.template read_le<std::uint64_t>(old_rsp);
        if (!target) return StepResult::MemoryFault;
        state.gpr[rsp_index] = old_rsp + sizeof(std::uint64_t);
        state.rip = *target;
        return StepResult::Executed;
    }

    // Short relative JMP and all 16 x86 condition-code branches.
    if (opcode == 0xeb || (opcode >= 0x70 && opcode <= 0x7f)) {
        std::uint8_t raw_displacement = 0;
        if (!fetch8(memory, pc, raw_displacement)) return StepResult::MemoryFault;
        const auto displacement = static_cast<std::int8_t>(raw_displacement);
        const bool take = opcode == 0xeb ||
                          condition_true(static_cast<std::uint8_t>(opcode & 0x0fU),
                                         state.rflags);
        state.rip = take ? pc + static_cast<std::int64_t>(displacement) : pc;
        return StepResult::Executed;
    }

    bool rex_w = false;
    bool rex_r = false;
    bool rex_x = false;
    bool rex_b = false;
    bool rex_present = false;
    if (opcode >= 0x40 && opcode <= 0x4f) {
        rex_present = true;
        rex_w = (opcode & 0x08U) != 0;
        rex_r = (opcode & 0x04U) != 0;
        rex_x = (opcode & 0x02U) != 0;
        rex_b = (opcode & 0x01U) != 0;
        if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;
    }

    // Near relative JZ/JNZ (0F 84 / 0F 85), with a signed 32-bit offset.
    if (opcode == 0x0f) {
        std::uint8_t second = 0;
        if (!fetch8(memory, pc, second)) return StepResult::MemoryFault;
        // CPUID: expose a conservative synthetic Meridian-XS profile.
        if (second == 0xa2) {
            const std::uint32_t leaf = static_cast<std::uint32_t>(state.gpr[0]);
            const std::uint32_t subleaf = static_cast<std::uint32_t>(state.gpr[1]);
            std::uint32_t eax = 0;
            std::uint32_t ebx = 0;
            std::uint32_t ecx = 0;
            std::uint32_t edx = 0;
            constexpr std::uint32_t vendor_ebx = 0x6972654dU; // "Meri"
            constexpr std::uint32_t vendor_edx = 0x6e616964U; // "dian"
            constexpr std::uint32_t vendor_ecx = 0x20205358U; // "XS  "
            if (leaf == 0) {
                eax = 1;
                ebx = vendor_ebx;
                edx = vendor_edx;
                ecx = vendor_ecx;
            } else if (leaf == 1) {
                eax = 0x00000600U;
                edx = (1U << 4U) | (1U << 5U) | (1U << 15U); // TSC, MSR, CMOV
            } else if (leaf == 0x80000000U) {
                eax = 0x80000001U;
                ebx = vendor_ebx;
                edx = vendor_edx;
                ecx = vendor_ecx;
            } else if (leaf == 0x80000001U) {
                edx = (1U << 11U) | (1U << 20U) | (1U << 29U); // SYSCALL, NX, LM
            }
            (void)subleaf;
            state.gpr[0] = eax;
            state.gpr[3] = ebx;
            state.gpr[1] = ecx;
            state.gpr[2] = edx;
            state.rip = pc;
            return StepResult::Executed;
        }

        // RDTSC returns the deterministic guest TSC in EDX:EAX.
        if (second == 0x31) {
            if ((state.control_registers[4] & (1ULL << 2U)) != 0 && state.cpl != 0)
                return StepResult::GeneralProtectionFault;
            const std::uint64_t value = state.tsc++;
            state.gpr[0] = static_cast<std::uint32_t>(value);
            state.gpr[2] = static_cast<std::uint32_t>(value >> 32U);
            state.rip = pc;
            return StepResult::Executed;
        }

        // SYSCALL / SYSRETQ: model the long-mode register and privilege transition.
        if (second == 0x05) {
            if ((state.efer & 1ULL) == 0 || !canonical_address(state.lstar))
                return StepResult::GeneralProtectionFault;
            state.gpr[1] = pc;       // RCX receives the return RIP.
            state.gpr[11] = state.rflags; // R11 receives the saved flags.
            state.rflags &= ~state.sfmask;
            state.rflags |= 0x2;
            const std::uint16_t kernel_cs =
                static_cast<std::uint16_t>((state.star >> 32U) & 0xfffcU);
            state.cs = kernel_cs;
            state.ss = static_cast<std::uint16_t>(kernel_cs + 8U);
            state.cpl = 0;
            state.rip = state.lstar;
            return StepResult::Executed;
        }
        if (second == 0x07) {
            if (state.cpl != 0 || (state.efer & 1ULL) == 0 ||
                !canonical_address(state.gpr[1]))
                return StepResult::GeneralProtectionFault;
            const std::uint16_t user_base =
                static_cast<std::uint16_t>((state.star >> 48U) & 0xfffcU);
            state.cs = static_cast<std::uint16_t>((user_base + 16U) | 3U);
            state.ss = static_cast<std::uint16_t>((user_base + 8U) | 3U);
            state.cpl = 3;
            state.rip = state.gpr[1];
            state.rflags = state.gpr[11] | 0x2;
            return StepResult::Executed;
        }

        // RDMSR / WRMSR. Only the explicitly modeled MSR set is implemented.
        if (second == 0x30 || second == 0x32) {
            if (state.cpl != 0) return StepResult::PrivilegeFault;
            const std::uint32_t index = static_cast<std::uint32_t>(state.gpr[1]);
            if (second == 0x32) {
                std::uint64_t value = 0;
                if (!read_msr(state, index, value))
                    return StepResult::GeneralProtectionFault;
                state.gpr[0] = static_cast<std::uint32_t>(value);
                state.gpr[2] = static_cast<std::uint32_t>(value >> 32U);
            } else {
                const std::uint64_t value =
                    (static_cast<std::uint64_t>(static_cast<std::uint32_t>(state.gpr[2])) << 32U) |
                    static_cast<std::uint32_t>(state.gpr[0]);
                if (!write_msr(state, index, value))
                    return StepResult::GeneralProtectionFault;
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // INVLPG m: invalidate the translation for the supplied linear address.
        if (second == 0x01) {
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if (modrm == 0xf9) { // RDTSCP
                if ((state.control_registers[4] & (1ULL << 2U)) != 0 &&
                    state.cpl != 0) return StepResult::GeneralProtectionFault;
                const std::uint64_t value = state.tsc++;
                state.gpr[0] = static_cast<std::uint32_t>(value);
                state.gpr[2] = static_cast<std::uint32_t>(value >> 32U);
                state.gpr[1] = static_cast<std::uint32_t>(state.tsc_aux);
                state.rip = pc;
                return StepResult::Executed;
            }
            if (modrm == 0xf8) { // SWAPGS
                if (state.cpl != 0) return StepResult::PrivilegeFault;
                std::swap(state.gs_base, state.kernel_gs_base);
                state.rip = pc;
                return StepResult::Executed;
            }
            if (((modrm >> 3U) & 7U) != 7U || (modrm >> 6U) == 3U)
                return StepResult::UnsupportedInstruction;
            if (state.cpl != 0) return StepResult::PrivilegeFault;
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, 0, address))
                return StepResult::MemoryFault;
            if constexpr (requires(Memory& candidate) {
                candidate.invalidate_page(std::uint64_t{});
            }) {
                memory.invalidate_page(address);
                state.rip = pc;
                return StepResult::Executed;
            } else {
                return StepResult::UnsupportedInstruction;
            }
        }

        // MOV between a general-purpose register and CR0/CR2/CR3/CR4/CR8.
        if (second == 0x20 || second == 0x22) {
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if ((modrm >> 6U) != 3U) return StepResult::UnsupportedInstruction;
            if (state.cpl != 0) return StepResult::PrivilegeFault;
            const std::size_t control = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                        (rex_r ? 8U : 0U);
            const std::size_t general = static_cast<std::size_t>(modrm & 7U) +
                                        (rex_b ? 8U : 0U);
            if (control != 0 && control != 2 && control != 3 &&
                control != 4 && control != 8)
                return StepResult::UnsupportedInstruction;
            if (second == 0x20) {
                state.gpr[general] = state.control_registers[control];
            } else {
                state.control_registers[control] = state.gpr[general];
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // CMOVcc r16/r32/r64, r/m. Register-direct source forms are supported.
        if (second >= 0x40 && second <= 0x4f) {
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if ((modrm & 0xc0U) != 0xc0U)
                return StepResult::UnsupportedInstruction;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t source =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            if (condition_true(static_cast<std::uint8_t>(second & 0x0fU), state.rflags)) {
                state.gpr[destination] = rex_w ? state.gpr[source]
                    : static_cast<std::uint32_t>(state.gpr[source]);
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // SETcc r/m8. This first implementation supports register destinations.
        if (second >= 0x90 && second <= 0x9f) {
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
            const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
            const std::uint8_t value = condition_true(
                static_cast<std::uint8_t>(second & 0x0fU), state.rflags) ? 1U : 0U;
            if (mod != 3) {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                if (!memory.template write_le<std::uint8_t>(address, value))
                    return StepResult::MemoryFault;
            } else if (!rex_present && rm_low >= 4) {
                const std::size_t reg = static_cast<std::size_t>(rm_low - 4U);
                state.gpr[reg] = (state.gpr[reg] & ~(0xffULL << 8U)) |
                                 (static_cast<std::uint64_t>(value) << 8U);
            } else {
                const std::size_t reg = static_cast<std::size_t>(rm_low) +
                                        (rex_b ? 8U : 0U);
                state.gpr[reg] = (state.gpr[reg] & ~0xffULL) | value;
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        if (second == 0xb6 || second == 0xb7 ||
            second == 0xbe || second == 0xbf) {
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const bool word_source = second == 0xb7 || second == 0xbf;
            const bool sign_extend = second == 0xbe || second == 0xbf;
            std::uint64_t value = 0;

            if (mod == 3) {
                const std::size_t source =
                    static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
                if (word_source) {
                    value = static_cast<std::uint16_t>(state.gpr[source]);
                } else if (rex_present) {
                    value = static_cast<std::uint8_t>(state.gpr[source]);
                } else {
                    const std::uint8_t low = static_cast<std::uint8_t>(modrm & 7U);
                    value = low >= 4
                        ? static_cast<std::uint8_t>(state.gpr[low - 4U] >> 8U)
                        : static_cast<std::uint8_t>(state.gpr[source]);
                }
            } else {
                const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
                std::uint64_t base_value = 0;
                std::uint64_t index_value = 0;
                unsigned scale_shift = 0;
                bool has_base = true;
                bool rip_relative = false;
                if (rm_low == 4) {
                    std::uint8_t sib = 0;
                    if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
                    scale_shift = static_cast<unsigned>(sib >> 6U);
                    const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
                    const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
                    if (index_low != 4 || rex_x) {
                        const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                                      (rex_x ? 8U : 0U);
                        index_value = state.gpr[index_reg];
                    }
                    if (mod == 0 && base_low == 5) {
                        has_base = false;
                    } else {
                        const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                                     (rex_b ? 8U : 0U);
                        base_value = state.gpr[base_reg];
                    }
                } else if (mod == 0 && rm_low == 5) {
                    rip_relative = true;
                } else {
                    const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                                 (rex_b ? 8U : 0U);
                    base_value = state.gpr[base_reg];
                }

                std::int64_t displacement = 0;
                if (mod == 1) {
                    std::uint8_t raw = 0;
                    if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
                    displacement = std::bit_cast<std::int8_t>(raw);
                } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
                    const auto raw = memory.template fetch_le<std::uint32_t>(pc);
                    if (!raw) return StepResult::MemoryFault;
                    displacement = std::bit_cast<std::int32_t>(*raw);
                    pc += 4;
                }
                const std::uint64_t address =
                    (rip_relative ? pc : (has_base ? base_value : 0)) +
                    (index_value << scale_shift) + static_cast<std::uint64_t>(displacement);
                if (word_source) {
                    const auto loaded = memory.template read_le<std::uint16_t>(address);
                    if (!loaded) return StepResult::MemoryFault;
                    value = *loaded;
                } else {
                    const auto loaded = memory.template read_le<std::uint8_t>(address);
                    if (!loaded) return StepResult::MemoryFault;
                    value = *loaded;
                }
            }

            if (sign_extend) {
                const std::int64_t signed_value = word_source
                    ? static_cast<std::int16_t>(static_cast<std::uint16_t>(value))
                    : static_cast<std::int8_t>(static_cast<std::uint8_t>(value));
                value = static_cast<std::uint64_t>(signed_value);
            }
            state.gpr[destination] = rex_w ? value : static_cast<std::uint32_t>(value);
            state.rip = pc;
            return StepResult::Executed;
        }
        if (second < 0x80 || second > 0x8f)
            return StepResult::UnsupportedInstruction;
        const auto raw_displacement = memory.template fetch_le<std::uint32_t>(pc);
        if (!raw_displacement) return StepResult::MemoryFault;
        pc += 4;
        const auto displacement = std::bit_cast<std::int32_t>(*raw_displacement);
        const bool take = condition_true(static_cast<std::uint8_t>(second & 0x0fU),
                                         state.rflags);
        state.rip = take ? pc + static_cast<std::int64_t>(displacement) : pc;
        return StepResult::Executed;
    }

    // PUSH/POP 64-bit general-purpose registers. REX.B selects R8-R15.
    if (opcode >= 0x50 && opcode <= 0x5f) {
        const bool is_push = opcode <= 0x57;
        const std::size_t reg = static_cast<std::size_t>(
            opcode - (is_push ? 0x50 : 0x58)) + (rex_b ? 8U : 0U);
        if (is_push) {
            const std::uint64_t value = state.gpr[reg];
            if (!push64(state, memory, value)) return StepResult::MemoryFault;
        } else {
            const std::uint64_t old_rsp = state.gpr[rsp_index];
            if (old_rsp > UINT64_MAX - sizeof(std::uint64_t)) return StepResult::MemoryFault;
            const auto value = memory.template read_le<std::uint64_t>(old_rsp);
            if (!value) return StepResult::MemoryFault;
            state.gpr[rsp_index] = old_rsp + sizeof(std::uint64_t);
            state.gpr[reg] = *value;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // MOV r64, imm64 / MOV r32, imm32. REX.B selects R8-R15.
    if (opcode >= 0xb8 && opcode <= 0xbf) {
        const std::size_t reg = static_cast<std::size_t>(opcode - 0xb8) + (rex_b ? 8U : 0U);
        if (rex_w) {
            const auto immediate = memory.template fetch_le<std::uint64_t>(pc);
            if (!immediate) return StepResult::MemoryFault;
            state.gpr[reg] = *immediate;
            pc += 8;
        } else {
            const auto immediate = memory.template fetch_le<std::uint32_t>(pc);
            if (!immediate) return StepResult::MemoryFault;
            state.gpr[reg] = *immediate;
            pc += 4;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // Group-1 immediate arithmetic: ADD, OR, AND, SUB, XOR, and CMP.
    // Supports imm32 (81) and sign-extended imm8 (83), with register or memory destinations.
    if (opcode == 0x81 || opcode == 0x83) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        const std::uint8_t operation = static_cast<std::uint8_t>((modrm >> 3U) & 7U);
        if (operation != 0 && operation != 1 && operation != 4 &&
            operation != 5 && operation != 6 && operation != 7)
            return StepResult::UnsupportedInstruction;

        const std::size_t destination = static_cast<std::size_t>(modrm & 7U) +
                                        (rex_b ? 8U : 0U);
        const std::size_t immediate_size = opcode == 0x83 ? 1U : 4U;
        std::uint64_t address = 0;
        if (mod != 3 &&
            !decode_effective_address(state, memory, pc, modrm, rex_b, rex_x,
                                      immediate_size, address))
            return StepResult::MemoryFault;

        std::uint32_t raw_immediate = 0;
        if (opcode == 0x83) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            raw_immediate = static_cast<std::uint32_t>(
                static_cast<std::int32_t>(std::bit_cast<std::int8_t>(raw)));
        } else {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            raw_immediate = *raw;
            pc += 4;
        }

        if (rex_w) {
            const std::int64_t signed_immediate = opcode == 0x83
                ? static_cast<std::int64_t>(std::bit_cast<std::int8_t>(
                      static_cast<std::uint8_t>(raw_immediate)))
                : static_cast<std::int64_t>(std::bit_cast<std::int32_t>(raw_immediate));
            const std::uint64_t rhs = static_cast<std::uint64_t>(signed_immediate);
            std::uint64_t lhs = 0;
            if (mod == 3) {
                lhs = state.gpr[destination];
            } else {
                const auto loaded = memory.template read_le<std::uint64_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                lhs = *loaded;
            }
            std::uint64_t result = 0;
            switch (operation) {
            case 0: result = lhs + rhs; update_add_flags(state, lhs, rhs, result); break;
            case 1: result = lhs | rhs; update_logic_flags(state, result); break;
            case 4: result = lhs & rhs; update_logic_flags(state, result); break;
            case 5: result = lhs - rhs; update_sub_flags(state, lhs, rhs, result); break;
            case 6: result = lhs ^ rhs; update_logic_flags(state, result); break;
            case 7: result = lhs - rhs; update_sub_flags(state, lhs, rhs, result); break;
            default: return StepResult::UnsupportedInstruction;
            }
            if (operation != 7) {
                if (mod == 3) state.gpr[destination] = result;
                else if (!memory.template write_le<std::uint64_t>(address, result))
                    return StepResult::MemoryFault;
            }
        } else {
            const std::uint32_t rhs = raw_immediate;
            std::uint32_t lhs = 0;
            if (mod == 3) {
                lhs = static_cast<std::uint32_t>(state.gpr[destination]);
            } else {
                const auto loaded = memory.template read_le<std::uint32_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                lhs = *loaded;
            }
            std::uint32_t result = 0;
            switch (operation) {
            case 0: result = lhs + rhs; update_add_flags32(state, lhs, rhs, result); break;
            case 1: result = lhs | rhs; update_logic_flags32(state, result); break;
            case 4: result = lhs & rhs; update_logic_flags32(state, result); break;
            case 5: result = lhs - rhs; update_sub_flags32(state, lhs, rhs, result); break;
            case 6: result = lhs ^ rhs; update_logic_flags32(state, result); break;
            case 7: result = lhs - rhs; update_sub_flags32(state, lhs, rhs, result); break;
            default: return StepResult::UnsupportedInstruction;
            }
            if (operation != 7) {
                if (mod == 3) state.gpr[destination] = result;
                else if (!memory.template write_le<std::uint32_t>(address, result))
                    return StepResult::MemoryFault;
            }
        }

        state.rip = pc;
        return StepResult::Executed;
    }

    // MOVSXD r32/r64, r/m32.
    if (opcode == 0x63) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        const std::size_t destination = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                        (rex_r ? 8U : 0U);
        std::uint32_t raw_value = 0;
        if (mod == 3) {
            const std::size_t source = static_cast<std::size_t>(modrm & 7U) +
                                       (rex_b ? 8U : 0U);
            raw_value = static_cast<std::uint32_t>(state.gpr[source]);
        } else {
            const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
            std::uint64_t base_value = 0;
            std::uint64_t index_value = 0;
            unsigned scale_shift = 0;
            bool has_base = true;
            bool rip_relative = false;
            if (rm_low == 4) {
                std::uint8_t sib = 0;
                if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
                scale_shift = static_cast<unsigned>(sib >> 6U);
                const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
                const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
                if (index_low != 4 || rex_x) {
                    const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                                  (rex_x ? 8U : 0U);
                    index_value = state.gpr[index_reg];
                }
                if (mod == 0 && base_low == 5) {
                    has_base = false;
                } else {
                    const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                                 (rex_b ? 8U : 0U);
                    base_value = state.gpr[base_reg];
                }
            } else if (mod == 0 && rm_low == 5) {
                rip_relative = true;
            } else {
                const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                             (rex_b ? 8U : 0U);
                base_value = state.gpr[base_reg];
            }
            std::int64_t displacement = 0;
            if (mod == 1) {
                std::uint8_t raw = 0;
                if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
                displacement = std::bit_cast<std::int8_t>(raw);
            } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
                const auto raw = memory.template fetch_le<std::uint32_t>(pc);
                if (!raw) return StepResult::MemoryFault;
                displacement = std::bit_cast<std::int32_t>(*raw);
                pc += 4;
            }
            const std::uint64_t address =
                (rip_relative ? pc : (has_base ? base_value : 0)) +
                (index_value << scale_shift) + static_cast<std::uint64_t>(displacement);
            const auto loaded = memory.template read_le<std::uint32_t>(address);
            if (!loaded) return StepResult::MemoryFault;
            raw_value = *loaded;
        }
        const auto value = std::bit_cast<std::int32_t>(raw_value);
        state.gpr[destination] = rex_w
            ? static_cast<std::uint64_t>(static_cast<std::int64_t>(value))
            : static_cast<std::uint32_t>(value);
        state.rip = pc;
        return StepResult::Executed;
    }

    // LEA r32/r64, m: calculate the effective address without reading memory.
    if (opcode == 0x8d) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        if (mod == 3) return StepResult::UnsupportedInstruction;

        const std::size_t destination = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                        (rex_r ? 8U : 0U);
        const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
        std::uint64_t base_value = 0;
        std::uint64_t index_value = 0;
        unsigned scale_shift = 0;
        bool has_base = true;
        bool rip_relative = false;

        if (rm_low == 4) {
            std::uint8_t sib = 0;
            if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
            scale_shift = static_cast<unsigned>(sib >> 6U);
            const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
            const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
            if (index_low != 4 || rex_x) {
                const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                              (rex_x ? 8U : 0U);
                index_value = state.gpr[index_reg];
            }
            if (mod == 0 && base_low == 5) {
                has_base = false;
            } else {
                const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                             (rex_b ? 8U : 0U);
                base_value = state.gpr[base_reg];
            }
        } else if (mod == 0 && rm_low == 5) {
            rip_relative = true;
        } else {
            const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                         (rex_b ? 8U : 0U);
            base_value = state.gpr[base_reg];
        }

        std::int64_t displacement = 0;
        if (mod == 1) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int8_t>(raw);
        } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int32_t>(*raw);
            pc += 4;
        }

        const std::uint64_t effective_address =
            (rip_relative ? pc : (has_base ? base_value : 0)) +
            (index_value << scale_shift) + static_cast<std::uint64_t>(displacement);
        state.gpr[destination] = rex_w ? effective_address
            : static_cast<std::uint32_t>(effective_address);
        state.rip = pc;
        return StepResult::Executed;
    }



    // MOV r/m32, imm32 and MOV r/m64, sign-extended imm32.
    if (opcode == 0xc7) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if (((modrm >> 3U) & 7U) != 0) return StepResult::UnsupportedInstruction;
        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);

        if (mod == 3) {
            const std::size_t destination =
                static_cast<std::size_t>(rm_low) + (rex_b ? 8U : 0U);
            const auto raw_immediate = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw_immediate) return StepResult::MemoryFault;
            pc += 4;
            if (rex_w) {
                const auto immediate = std::bit_cast<std::int32_t>(*raw_immediate);
                state.gpr[destination] = static_cast<std::uint64_t>(
                    static_cast<std::int64_t>(immediate));
            } else {
                state.gpr[destination] = *raw_immediate;
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        std::uint64_t base_value = 0;
        std::uint64_t index_value = 0;
        unsigned scale_shift = 0;
        bool has_base = true;
        bool rip_relative = false;
        if (rm_low == 4) {
            std::uint8_t sib = 0;
            if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
            scale_shift = static_cast<unsigned>(sib >> 6U);
            const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
            const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
            if (index_low != 4 || rex_x) {
                const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                              (rex_x ? 8U : 0U);
                index_value = state.gpr[index_reg];
            }
            if (mod == 0 && base_low == 5) {
                has_base = false;
            } else {
                const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                             (rex_b ? 8U : 0U);
                base_value = state.gpr[base_reg];
            }
        } else if (mod == 0 && rm_low == 5) {
            rip_relative = true;
        } else {
            const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                         (rex_b ? 8U : 0U);
            base_value = state.gpr[base_reg];
        }

        std::int64_t displacement = 0;
        if (mod == 1) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int8_t>(raw);
        } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int32_t>(*raw);
            pc += 4;
        }
        const auto raw_immediate = memory.template fetch_le<std::uint32_t>(pc);
        if (!raw_immediate) return StepResult::MemoryFault;
        pc += 4;
        // RIP-relative addressing is based on the end of the entire instruction,
        // including the immediate field.
        const std::uint64_t address =
            (rip_relative ? pc : (has_base ? base_value : 0)) +
            (index_value << scale_shift) + static_cast<std::uint64_t>(displacement);
        if (rex_w) {
            const auto immediate = std::bit_cast<std::int32_t>(*raw_immediate);
            const auto value = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(immediate));
            if (!memory.template write_le<std::uint64_t>(address, value))
                return StepResult::MemoryFault;
        } else if (!memory.template write_le<std::uint32_t>(address, *raw_immediate)) {
            return StepResult::MemoryFault;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // 32-bit and 64-bit MOV between registers and supported memory operands.
    // Supports base/displacement, SIB, and RIP-relative forms.
    if (opcode == 0x8b || opcode == 0x89) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;

        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        const std::size_t reg = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                (rex_r ? 8U : 0U);
        const std::size_t rm = static_cast<std::size_t>(modrm & 7U) +
                               (rex_b ? 8U : 0U);

        if (mod == 3) {
            if (opcode == 0x8b) {
                state.gpr[reg] = rex_w ? state.gpr[rm] :
                    static_cast<std::uint32_t>(state.gpr[rm]);
            } else {
                state.gpr[rm] = rex_w ? state.gpr[reg] :
                    static_cast<std::uint32_t>(state.gpr[reg]);
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
        std::uint64_t base_value = 0;
        std::uint64_t index_value = 0;
        unsigned scale_shift = 0;
        bool has_base = true;
        bool rip_relative = false;

        if (rm_low == 4) {
            std::uint8_t sib = 0;
            if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
            scale_shift = static_cast<unsigned>(sib >> 6U);
            const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
            const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
            if (index_low != 4 || rex_x) {
                const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                              (rex_x ? 8U : 0U);
                index_value = state.gpr[index_reg];
            }
            if (mod == 0 && base_low == 5) {
                has_base = false;
            } else {
                const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                             (rex_b ? 8U : 0U);
                base_value = state.gpr[base_reg];
            }
        } else {
            if (mod == 0 && rm_low == 5) {
                // In 64-bit mode this encoding is RIP-relative; REX.B is ignored.
                rip_relative = true;
            } else {
                base_value = state.gpr[rm];
            }
        }

        std::int64_t displacement = 0;
        if (mod == 1) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int8_t>(raw);
        } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int32_t>(*raw);
            pc += 4;
        }

        // RIP-relative offsets are based on the address immediately after
        // the complete instruction, including its displacement.
        const std::uint64_t address = (rip_relative ? pc : (has_base ? base_value : 0)) +
                                      (index_value << scale_shift) +
                                      static_cast<std::uint64_t>(displacement);
        if (opcode == 0x8b) {
            if (rex_w) {
                const auto value = memory.template read_le<std::uint64_t>(address);
                if (!value) return StepResult::MemoryFault;
                state.gpr[reg] = *value;
            } else {
                const auto value = memory.template read_le<std::uint32_t>(address);
                if (!value) return StepResult::MemoryFault;
                state.gpr[reg] = *value;
            }
        } else {
            if (rex_w) {
                if (!memory.template write_le<std::uint64_t>(address, state.gpr[reg]))
                    return StepResult::MemoryFault;
            } else {
                if (!memory.template write_le<std::uint32_t>(
                        address, static_cast<std::uint32_t>(state.gpr[reg])))
                    return StepResult::MemoryFault;
            }
        }

        state.rip = pc;
        return StepResult::Executed;
    }

    // Register-to-register MOV with 32-bit operand size. Writes to a
    // 32-bit destination zero-extend into the corresponding 64-bit GPR.
    // Memory forms remain unsupported in this instruction subset.
    if (!rex_w && (opcode == 0x89 || opcode == 0x8b)) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if ((modrm & 0xc0U) != 0xc0U) return StepResult::UnsupportedInstruction;

        const std::size_t source = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                   (rex_r ? 8U : 0U);
        const std::size_t destination = static_cast<std::size_t>(modrm & 7U) +
                                       (rex_b ? 8U : 0U);
        if (opcode == 0x89) {
            state.gpr[destination] = static_cast<std::uint32_t>(state.gpr[source]);
        } else {
            state.gpr[source] = static_cast<std::uint32_t>(state.gpr[destination]);
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // TEST r/m32, r32 and TEST r/m64, r64 (register-only forms).
    // Updates logical flags without modifying either operand.
    if (opcode == 0x85) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if ((modrm & 0xc0U) != 0xc0U) return StepResult::UnsupportedInstruction;

        const std::size_t source = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                   (rex_r ? 8U : 0U);
        const std::size_t destination = static_cast<std::size_t>(modrm & 7U) +
                                        (rex_b ? 8U : 0U);
        if (rex_w) {
            update_logic_flags(state, state.gpr[destination] & state.gpr[source]);
        } else {
            const auto lhs = static_cast<std::uint32_t>(state.gpr[destination]);
            const auto rhs = static_cast<std::uint32_t>(state.gpr[source]);
            update_logic_flags32(state, lhs & rhs);
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // ADD/SUB/XOR/CMP between a register and a memory destination.
    // This initial memory arithmetic subset supports [base], [base+disp8],
    // and [base+disp32] addressing; SIB and RIP-relative arithmetic follow later.
    if (opcode == 0x01 || opcode == 0x09 || opcode == 0x21 ||
        opcode == 0x29 || opcode == 0x31 || opcode == 0x39) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t mod = static_cast<std::uint8_t>(modrm >> 6U);
        if (mod == 3) {
            const std::size_t source_reg = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                           (rex_r ? 8U : 0U);
            const std::size_t destination_reg = static_cast<std::size_t>(modrm & 7U) +
                                                (rex_b ? 8U : 0U);
            if (rex_w) {
                const std::uint64_t lhs = state.gpr[destination_reg];
                const std::uint64_t rhs = state.gpr[source_reg];
                std::uint64_t result = 0;
                switch (opcode) {
                case 0x01: result = lhs + rhs; update_add_flags(state, lhs, rhs, result); state.gpr[destination_reg] = result; break;
                case 0x09: result = lhs | rhs; update_logic_flags(state, result); state.gpr[destination_reg] = result; break;
                case 0x21: result = lhs & rhs; update_logic_flags(state, result); state.gpr[destination_reg] = result; break;
                case 0x29: result = lhs - rhs; update_sub_flags(state, lhs, rhs, result); state.gpr[destination_reg] = result; break;
                case 0x31: result = lhs ^ rhs; update_logic_flags(state, result); state.gpr[destination_reg] = result; break;
                case 0x39: result = lhs - rhs; update_sub_flags(state, lhs, rhs, result); break;
                default: return StepResult::UnsupportedInstruction;
                }
            } else {
                const std::uint32_t lhs = static_cast<std::uint32_t>(state.gpr[destination_reg]);
                const std::uint32_t rhs = static_cast<std::uint32_t>(state.gpr[source_reg]);
                std::uint32_t result = 0;
                switch (opcode) {
                case 0x01: result = lhs + rhs; update_add_flags32(state, lhs, rhs, result); state.gpr[destination_reg] = result; break;
                case 0x09: result = lhs | rhs; update_logic_flags32(state, result); state.gpr[destination_reg] = result; break;
                case 0x21: result = lhs & rhs; update_logic_flags32(state, result); state.gpr[destination_reg] = result; break;
                case 0x29: result = lhs - rhs; update_sub_flags32(state, lhs, rhs, result); state.gpr[destination_reg] = result; break;
                case 0x31: result = lhs ^ rhs; update_logic_flags32(state, result); state.gpr[destination_reg] = result; break;
                case 0x39: result = lhs - rhs; update_sub_flags32(state, lhs, rhs, result); break;
                default: return StepResult::UnsupportedInstruction;
                }
            }
            state.rip = pc;
            return StepResult::Executed;
        }
        const std::uint8_t rm_low = static_cast<std::uint8_t>(modrm & 7U);
        const std::size_t source = static_cast<std::size_t>((modrm >> 3U) & 7U) +
                                   (rex_r ? 8U : 0U);
        std::uint64_t base_value = 0;
        std::uint64_t index_value = 0;
        unsigned scale_shift = 0;
        bool has_base = true;
        bool rip_relative = false;

        if (rm_low == 4) {
            std::uint8_t sib = 0;
            if (!fetch8(memory, pc, sib)) return StepResult::MemoryFault;
            scale_shift = static_cast<unsigned>(sib >> 6U);
            const std::uint8_t index_low = static_cast<std::uint8_t>((sib >> 3U) & 7U);
            const std::uint8_t base_low = static_cast<std::uint8_t>(sib & 7U);
            if (index_low != 4 || rex_x) {
                const std::size_t index_reg = static_cast<std::size_t>(index_low) +
                                              (rex_x ? 8U : 0U);
                index_value = state.gpr[index_reg];
            }
            if (mod == 0 && base_low == 5) {
                has_base = false;
            } else {
                const std::size_t base_reg = static_cast<std::size_t>(base_low) +
                                             (rex_b ? 8U : 0U);
                base_value = state.gpr[base_reg];
            }
        } else if (mod == 0 && rm_low == 5) {
            rip_relative = true;
        } else {
            const std::size_t base_reg = static_cast<std::size_t>(rm_low) +
                                         (rex_b ? 8U : 0U);
            base_value = state.gpr[base_reg];
        }

        std::int64_t displacement = 0;
        if (mod == 1) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int8_t>(raw);
        } else if (mod == 2 || (mod == 0 && (!has_base || rip_relative))) {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            displacement = std::bit_cast<std::int32_t>(*raw);
            pc += 4;
        }
        const std::uint64_t address = (rip_relative ? pc : (has_base ? base_value : 0)) +
                                      (index_value << scale_shift) +
                                      static_cast<std::uint64_t>(displacement);
        if (rex_w) {
            const auto loaded = memory.template read_le<std::uint64_t>(address);
            if (!loaded) return StepResult::MemoryFault;
            const std::uint64_t lhs = *loaded;
            const std::uint64_t rhs = state.gpr[source];
            std::uint64_t result = 0;
            switch (opcode) {
            case 0x01:
                result = lhs + rhs;
                update_add_flags(state, lhs, rhs, result);
                break;
            case 0x09:
                result = lhs | rhs;
                update_logic_flags(state, result);
                break;
            case 0x21:
                result = lhs & rhs;
                update_logic_flags(state, result);
                break;
            case 0x29:
                result = lhs - rhs;
                update_sub_flags(state, lhs, rhs, result);
                break;
            case 0x31:
                result = lhs ^ rhs;
                update_logic_flags(state, result);
                break;
            case 0x39:
                result = lhs - rhs;
                update_sub_flags(state, lhs, rhs, result);
                break;
            default:
                return StepResult::UnsupportedInstruction;
            }
            if (opcode != 0x39 && !memory.template write_le<std::uint64_t>(address, result))
                return StepResult::MemoryFault;
        } else {
            const auto loaded = memory.template read_le<std::uint32_t>(address);
            if (!loaded) return StepResult::MemoryFault;
            const std::uint32_t lhs = *loaded;
            const std::uint32_t rhs = static_cast<std::uint32_t>(state.gpr[source]);
            std::uint32_t result = 0;
            switch (opcode) {
            case 0x01:
                result = lhs + rhs;
                update_add_flags32(state, lhs, rhs, result);
                break;
            case 0x09:
                result = lhs | rhs;
                update_logic_flags32(state, result);
                break;
            case 0x21:
                result = lhs & rhs;
                update_logic_flags32(state, result);
                break;
            case 0x29:
                result = lhs - rhs;
                update_sub_flags32(state, lhs, rhs, result);
                break;
            case 0x31:
                result = lhs ^ rhs;
                update_logic_flags32(state, result);
                break;
            case 0x39:
                result = lhs - rhs;
                update_sub_flags32(state, lhs, rhs, result);
                break;
            default:
                return StepResult::UnsupportedInstruction;
            }
            if (opcode != 0x39 && !memory.template write_le<std::uint32_t>(address, result))
                return StepResult::MemoryFault;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // Register-only 32-bit and 64-bit ADD, SUB, XOR, and CMP.
    // In 64-bit mode, default operand size is 32 bits unless REX.W is set.
    if (opcode == 0x01 || opcode == 0x09 || opcode == 0x21 ||
        opcode == 0x29 || opcode == 0x31 || opcode == 0x39) {
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if ((modrm & 0xc0U) != 0xc0U) return StepResult::UnsupportedInstruction;

        const std::size_t source = static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
        const std::size_t destination = static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
        if (!rex_w) {
            const std::uint32_t lhs = static_cast<std::uint32_t>(state.gpr[destination]);
            const std::uint32_t rhs = static_cast<std::uint32_t>(state.gpr[source]);
            std::uint32_t result = 0;
            switch (opcode) {
            case 0x01:
                result = lhs + rhs;
                update_add_flags32(state, lhs, rhs, result);
                state.gpr[destination] = result;
                break;
            case 0x09:
                result = lhs | rhs;
                update_logic_flags32(state, result);
                state.gpr[destination] = result;
                break;
            case 0x21:
                result = lhs & rhs;
                update_logic_flags32(state, result);
                state.gpr[destination] = result;
                break;
            case 0x29:
                result = lhs - rhs;
                update_sub_flags32(state, lhs, rhs, result);
                state.gpr[destination] = result;
                break;
            case 0x31:
                result = lhs ^ rhs;
                update_logic_flags32(state, result);
                state.gpr[destination] = result;
                break;
            case 0x39:
                result = lhs - rhs;
                update_sub_flags32(state, lhs, rhs, result);
                break;
            default:
                return StepResult::UnsupportedInstruction;
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        const std::uint64_t lhs = state.gpr[destination];
        const std::uint64_t rhs = state.gpr[source];
        std::uint64_t result = 0;

        switch (opcode) {
        case 0x01:
            result = lhs + rhs;
            update_add_flags(state, lhs, rhs, result);
            state.gpr[destination] = result;
            break;
        case 0x09:
            result = lhs | rhs;
            update_logic_flags(state, result);
            state.gpr[destination] = result;
            break;
        case 0x21:
            result = lhs & rhs;
            update_logic_flags(state, result);
            state.gpr[destination] = result;
            break;
        case 0x29:
            result = lhs - rhs;
            update_sub_flags(state, lhs, rhs, result);
            state.gpr[destination] = result;
            break;
        case 0x31:
            result = lhs ^ rhs;
            update_logic_flags(state, result);
            state.gpr[destination] = result;
            break;
        case 0x39:
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

StepResult Interpreter::step(CpuState& state, core::GuestMemory& memory) const noexcept {
    return step_impl(state, memory);
}

StepResult Interpreter::step(CpuState& state, core::VirtualAddressSpace& memory) const noexcept {
    return step_impl(state, memory);
}

StepResult Interpreter::step(CpuState& state, core::X64PagedMemory& memory) const noexcept {
    const auto synchronize_paging_context = [&]() noexcept {
        constexpr std::uint64_t cr0_wp = 1ULL << 16U;
        constexpr std::uint64_t efer_nxe = 1ULL << 11U;
        memory.set_context(state.control_registers[3], state.cpl == 3,
                           (state.control_registers[0] & cr0_wp) != 0,
                           (state.efer & efer_nxe) != 0);
    };
    synchronize_paging_context();
    const StepResult result = step_impl(state, memory);
    synchronize_paging_context();
    return result;
}

} // namespace meridian::cpu
