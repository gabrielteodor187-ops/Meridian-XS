#include "meridian/cpu/interpreter.hpp"

#include <bit>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cmath>

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

void update_common_flags16(CpuState& state, std::uint16_t result) noexcept {
    if (result == 0) state.rflags |= flag_zf;
    if ((result & 0x8000U) != 0) state.rflags |= flag_sf;
    if ((std::popcount(static_cast<std::uint8_t>(result)) & 1) == 0)
        state.rflags |= flag_pf;
}

void update_add_flags16(CpuState& state, std::uint16_t lhs, std::uint16_t rhs,
                        std::uint16_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (static_cast<std::uint32_t>(lhs) + rhs > 0xffffU) state.rflags |= flag_cf;
    if (((~(lhs ^ rhs) & (lhs ^ result)) & 0x8000U) != 0) state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags16(state, result);
}

void update_sub_flags16(CpuState& state, std::uint16_t lhs, std::uint16_t rhs,
                        std::uint16_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    if (lhs < rhs) state.rflags |= flag_cf;
    if ((((lhs ^ rhs) & (lhs ^ result)) & 0x8000U) != 0) state.rflags |= flag_of;
    if (((lhs ^ rhs ^ result) & 0x10U) != 0) state.rflags |= flag_af;
    update_common_flags16(state, result);
}

void update_logic_flags16(CpuState& state, std::uint16_t result) noexcept {
    state.rflags &= ~arithmetic_flags;
    update_common_flags16(state, result);
}


struct SignedMultiplyResult {
    std::uint64_t value{0};
    bool overflow{false};
};

SignedMultiplyResult signed_multiply(std::uint64_t lhs, std::uint64_t rhs,
                                     std::size_t width) noexcept {
    if (width == 8U) {
        const bool negative = ((lhs >> 63U) != 0) != ((rhs >> 63U) != 0);
        const std::uint64_t abs_lhs = (lhs >> 63U) != 0 ? (~lhs + 1U) : lhs;
        const std::uint64_t abs_rhs = (rhs >> 63U) != 0 ? (~rhs + 1U) : rhs;
        const std::uint64_t limit = negative ? (1ULL << 63U) : 0x7fffffffffffffffULL;
        const bool overflow = abs_lhs != 0 && abs_rhs > limit / abs_lhs;
        return {lhs * rhs, overflow};
    }
    const std::int64_t a = width == 2U
        ? std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lhs))
        : std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(lhs));
    const std::int64_t b = width == 2U
        ? std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs))
        : std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(rhs));
    const std::int64_t product = a * b;
    const std::int64_t minimum = width == 2U
        ? std::numeric_limits<std::int16_t>::min()
        : std::numeric_limits<std::int32_t>::min();
    const std::int64_t maximum = width == 2U
        ? std::numeric_limits<std::int16_t>::max()
        : std::numeric_limits<std::int32_t>::max();
    const std::uint64_t mask = width == 2U ? 0xffffULL : 0xffffffffULL;
    return {static_cast<std::uint64_t>(product) & mask,
            product < minimum || product > maximum};
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


struct VexPrefix {
    bool r{false};
    bool x{false};
    bool b{false};
    bool w{false};
    bool l{false};
    std::uint8_t map{0};
    std::uint8_t pp{0};
    std::uint8_t vvvv{0};
};

template <typename Memory>
StepResult execute_vex(CpuState& state, Memory& memory, std::uint64_t pc,
                       std::uint8_t opcode, const VexPrefix& vex) noexcept {
    if ((vex.map < 1 || vex.map > 3) || vex.w || vex.pp > 2)
        return StepResult::UnsupportedInstruction;
    if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
        (state.control_registers[4] & ((1ULL << 9U) | (1ULL << 18U))) !=
            ((1ULL << 9U) | (1ULL << 18U)) ||
        (state.xcr0 & 6ULL) != 6ULL)
        return StepResult::UnsupportedInstruction;

    const std::size_t width = vex.l ? 32U : 16U;
    if (vex.map == 3) { // VEX.0F3A map
        const bool permute_float = opcode == 0x06 && vex.pp == 0;
        const bool permute_integer = opcode == 0x46 && vex.pp == 1;
        if ((!permute_float && !permute_integer) || !vex.l)
            return StepResult::UnsupportedInstruction;
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::size_t destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (vex.r ? 8U : 0U);
        const std::size_t rm_index =
            static_cast<std::size_t>(modrm & 7U) + (vex.b ? 8U : 0U);
        const bool register_operand = (modrm >> 6U) == 3U;
        std::array<std::byte, 32> source2{};
        if (register_operand) {
            std::copy(state.xmm[rm_index].begin(), state.xmm[rm_index].end(),
                      source2.begin());
            std::copy(state.ymm_high[rm_index].begin(),
                      state.ymm_high[rm_index].end(), source2.begin() + 16);
        } else {
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          vex.b, vex.x, 1, address))
                return StepResult::MemoryFault;
            if (!memory.read(address, source2))
                return StepResult::MemoryFault;
        }
        std::uint8_t immediate = 0;
        if (!fetch8(memory, pc, immediate)) return StepResult::MemoryFault;
        std::array<std::byte, 32> source1{};
        std::copy(state.xmm[vex.vvvv].begin(), state.xmm[vex.vvvv].end(),
                  source1.begin());
        std::copy(state.ymm_high[vex.vvvv].begin(),
                  state.ymm_high[vex.vvvv].end(), source1.begin() + 16);
        std::array<std::byte, 32> result{};
        const std::array<const std::array<std::byte, 32>*, 4> choices{
            &source1, &source1, &source2, &source2
        };
        const std::array<std::size_t, 4> offsets{0, 16, 0, 16};
        if ((immediate & 0x08U) == 0) {
            const std::size_t selection = immediate & 3U;
            std::copy_n(choices[selection]->begin() + offsets[selection], 16,
                        result.begin());
        }
        if ((immediate & 0x80U) == 0) {
            const std::size_t selection = (immediate >> 4U) & 3U;
            std::copy_n(choices[selection]->begin() + offsets[selection], 16,
                        result.begin() + 16);
        }
        std::copy_n(result.begin(), 16, state.xmm[destination].begin());
        std::copy_n(result.begin() + 16, 16, state.ymm_high[destination].begin());
        state.rip = pc;
        return StepResult::Executed;
    }
    if (vex.map == 2) { // VEX.0F38 map
        if (opcode != 0x00 || vex.pp != 1 || vex.vvvv > 15U)
            return StepResult::UnsupportedInstruction; // VPSHUFB only.
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::size_t destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (vex.r ? 8U : 0U);
        const std::size_t rm_index =
            static_cast<std::size_t>(modrm & 7U) + (vex.b ? 8U : 0U);
        const bool register_operand = (modrm >> 6U) == 3U;
        std::array<std::byte, 32> controls{};
        if (register_operand) {
            std::copy(state.xmm[rm_index].begin(), state.xmm[rm_index].end(),
                      controls.begin());
            if (vex.l)
                std::copy(state.ymm_high[rm_index].begin(),
                          state.ymm_high[rm_index].end(), controls.begin() + 16);
        } else {
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          vex.b, vex.x, 0, address))
                return StepResult::MemoryFault;
            if (!memory.read(address,
                    std::span<std::byte>{controls}.first(width)))
                return StepResult::MemoryFault;
        }
        std::array<std::byte, 32> source{};
        std::copy(state.xmm[vex.vvvv].begin(), state.xmm[vex.vvvv].end(),
                  source.begin());
        if (vex.l)
            std::copy(state.ymm_high[vex.vvvv].begin(),
                      state.ymm_high[vex.vvvv].end(), source.begin() + 16);
        std::array<std::byte, 32> result{};
        for (std::size_t lane = 0; lane < width; lane += 16U) {
            for (std::size_t i = 0; i < 16; ++i) {
                const auto control =
                    std::to_integer<unsigned char>(controls[lane + i]);
                if ((control & 0x80U) != 0) {
                    result[lane + i] = std::byte{0};
                } else {
                    result[lane + i] = source[lane + (control & 0x0fU)];
                }
            }
        }
        std::copy_n(result.begin(), 16, state.xmm[destination].begin());
        if (vex.l)
            std::copy_n(result.begin() + 16, 16, state.ymm_high[destination].begin());
        else
            state.ymm_high[destination].fill(std::byte{0});
        state.rip = pc;
        return StepResult::Executed;
    }
    if (vex.map != 1) return StepResult::UnsupportedInstruction;
    if (opcode == 0x77) { // VZEROUPPER / VZEROALL
        if (vex.pp != 0 || vex.vvvv != 0x0fU) return StepResult::UnsupportedInstruction;
        if (vex.l) {
            for (auto& reg : state.xmm) reg.fill(std::byte{0});
            for (auto& reg : state.ymm_high) reg.fill(std::byte{0});
        } else {
            for (auto& reg : state.ymm_high) reg.fill(std::byte{0});
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    const bool pshufd = opcode == 0x70 && vex.pp == 1;
    const bool shufps = opcode == 0xc6 && vex.pp == 0;
    const bool shuffle = pshufd || shufps;
    const bool packed_compare = vex.pp == 1 &&
        (opcode == 0x74 || opcode == 0x75 || opcode == 0x76 ||
         opcode == 0x64 || opcode == 0x65 || opcode == 0x66);
    const bool movemask = opcode == 0xd7 && vex.pp == 1;
    const bool dqa_move = (opcode == 0x6f || opcode == 0x7f) && vex.pp == 1;
    const bool dqu_move = (opcode == 0x6f || opcode == 0x7f) && vex.pp == 2;
    const bool is_move_load = opcode == 0x10 || opcode == 0x28 ||
                              (opcode == 0x6f && (dqa_move || dqu_move));
    const bool is_move_store = opcode == 0x11 || opcode == 0x29 ||
                               (opcode == 0x7f && (dqa_move || dqu_move));
    const bool aligned_move = opcode == 0x28 || opcode == 0x29 || dqa_move;
    const bool is_and = opcode == 0x54 || (opcode == 0xdb && vex.pp == 1);
    const bool is_andn = opcode == 0x55;
    const bool is_or = opcode == 0x56 || (opcode == 0xeb && vex.pp == 1);
    const bool is_xor = opcode == 0x57 || (opcode == 0xef && vex.pp == 1);
    const bool is_add = opcode == 0x58;
    const bool is_sub = opcode == 0x5c;
    const bool is_mul = opcode == 0x59;
    const bool is_div = opcode == 0x5e;
    const bool is_integer_arithmetic = vex.pp == 1 &&
        (opcode == 0xfc || opcode == 0xfd || opcode == 0xfe || opcode == 0xd4 ||
         opcode == 0xf8 || opcode == 0xf9 || opcode == 0xfa || opcode == 0xfb ||
         opcode == 0xd5 || opcode == 0xec || opcode == 0xdc ||
         opcode == 0xed || opcode == 0xdd || opcode == 0xe8 ||
         opcode == 0xd8 || opcode == 0xe9 || opcode == 0xd9 ||
         opcode == 0xda || opcode == 0xde || opcode == 0xea ||
         opcode == 0xee || opcode == 0xe0 || opcode == 0xe3);
    if (vex.pp == 2 && !dqu_move)
        return StepResult::UnsupportedInstruction;
    if (is_move_load || is_move_store) {
        if (opcode == 0x10 || opcode == 0x11 ||
            opcode == 0x28 || opcode == 0x29) {
            if (vex.pp > 1) return StepResult::UnsupportedInstruction;
        } else if (!dqa_move && !dqu_move) {
            return StepResult::UnsupportedInstruction;
        }
    } else if (movemask) {
        if (vex.vvvv != 0x0fU) return StepResult::UnsupportedInstruction;
    } else if (!(shuffle || packed_compare || is_and || is_andn || is_or || is_xor ||
                 is_add || is_sub || is_mul || is_div ||
                 is_integer_arithmetic)) {
        return StepResult::UnsupportedInstruction;
    }
    if (((is_move_load || is_move_store) || pshufd) && vex.vvvv != 0x0fU)
        return StepResult::UnsupportedInstruction;

    std::uint8_t modrm = 0;
    if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
    if (movemask) {
        if ((modrm >> 6U) != 3U) return StepResult::UnsupportedInstruction;
        const std::size_t gpr_destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (vex.r ? 8U : 0U);
        const std::size_t vector_source =
            static_cast<std::size_t>(modrm & 7U) + (vex.b ? 8U : 0U);
        std::uint32_t mask = 0;
        const std::size_t byte_count = vex.l ? 32U : 16U;
        for (std::size_t i = 0; i < byte_count; ++i) {
            const std::byte value = i < 16U
                ? state.xmm[vector_source][i]
                : state.ymm_high[vector_source][i - 16U];
            if ((std::to_integer<unsigned char>(value) & 0x80U) != 0)
                mask |= 1U << i;
        }
        state.gpr[gpr_destination] = mask;
        state.rip = pc;
        return StepResult::Executed;
    }
    const std::size_t destination =
        static_cast<std::size_t>((modrm >> 3U) & 7U) + (vex.r ? 8U : 0U);
    const std::size_t rm_index =
        static_cast<std::size_t>(modrm & 7U) + (vex.b ? 8U : 0U);
    const bool register_operand = (modrm >> 6U) == 3U;
    std::uint64_t address = 0;

    std::array<std::byte, 32> source2{};
    if (register_operand) {
        std::copy(state.xmm[rm_index].begin(), state.xmm[rm_index].end(),
                  source2.begin());
        if (vex.l)
            std::copy(state.ymm_high[rm_index].begin(), state.ymm_high[rm_index].end(),
                      source2.begin() + 16);
    } else {
        if (!decode_effective_address(state, memory, pc, modrm,
                                      vex.b, vex.x, shuffle ? 1U : 0U, address))
            return StepResult::MemoryFault;
        if (aligned_move && (address & (width - 1U)) != 0)
            return StepResult::GeneralProtectionFault;
        if (is_move_load || shuffle || packed_compare || is_and || is_andn || is_or || is_xor ||
            is_add || is_sub || is_mul || is_div || is_integer_arithmetic) {
            if (!memory.read(address,
                    std::span<std::byte>{source2}.first(width)))
                return StepResult::MemoryFault;
        }
    }

    if (shuffle) {
        std::uint8_t immediate = 0;
        if (!fetch8(memory, pc, immediate)) return StepResult::MemoryFault;
        std::array<std::byte, 32> source1{};
        if (shufps) {
            std::copy(state.xmm[vex.vvvv].begin(), state.xmm[vex.vvvv].end(),
                      source1.begin());
            if (vex.l)
                std::copy(state.ymm_high[vex.vvvv].begin(),
                          state.ymm_high[vex.vvvv].end(), source1.begin() + 16);
        }
        std::array<std::byte, 32> result{};
        const auto read_dword = [](const std::array<std::byte, 32>& value,
                                   std::size_t offset) {
            std::uint32_t dword = 0;
            for (std::size_t byte = 0; byte < 4; ++byte)
                dword |= static_cast<std::uint32_t>(
                    std::to_integer<unsigned char>(value[offset + byte]))
                    << (byte * 8U);
            return dword;
        };
        const auto write_dword = [](std::array<std::byte, 32>& value,
                                    std::size_t offset, std::uint32_t dword) {
            for (std::size_t byte = 0; byte < 4; ++byte)
                value[offset + byte] =
                    static_cast<std::byte>((dword >> (byte * 8U)) & 0xffU);
        };
        for (std::size_t block = 0; block < width; block += 16U) {
            if (pshufd) {
                for (std::size_t lane = 0; lane < 4; ++lane) {
                    const std::size_t selected = (immediate >> (lane * 2U)) & 3U;
                    write_dword(result, block + lane * 4U,
                                read_dword(source2, block + selected * 4U));
                }
            } else {
                write_dword(result, block,
                            read_dword(source1, block + (immediate & 3U) * 4U));
                write_dword(result, block + 4U,
                            read_dword(source1, block + ((immediate >> 2U) & 3U) * 4U));
                write_dword(result, block + 8U,
                            read_dword(source2, block + ((immediate >> 4U) & 3U) * 4U));
                write_dword(result, block + 12U,
                            read_dword(source2, block + ((immediate >> 6U) & 3U) * 4U));
            }
        }
        std::copy_n(result.begin(), 16, state.xmm[destination].begin());
        if (vex.l)
            std::copy_n(result.begin() + 16, 16, state.ymm_high[destination].begin());
        else
            state.ymm_high[destination].fill(std::byte{0});
        state.rip = pc;
        return StepResult::Executed;
    }

    if (is_move_load) {
        std::copy_n(source2.begin(), 16, state.xmm[destination].begin());
        if (vex.l)
            std::copy_n(source2.begin() + 16, 16, state.ymm_high[destination].begin());
        else
            state.ymm_high[destination].fill(std::byte{0});
    } else if (is_move_store) {
        std::array<std::byte, 32> source{};
        std::copy(state.xmm[destination].begin(), state.xmm[destination].end(),
                  source.begin());
        if (vex.l)
            std::copy(state.ymm_high[destination].begin(),
                      state.ymm_high[destination].end(), source.begin() + 16);
        if (register_operand) {
            std::copy_n(source.begin(), 16, state.xmm[rm_index].begin());
            if (vex.l)
                std::copy_n(source.begin() + 16, 16, state.ymm_high[rm_index].begin());
            else
                state.ymm_high[rm_index].fill(std::byte{0});
        } else if (!memory.write(address,
                       std::span<const std::byte>{source}.first(width))) {
            return StepResult::MemoryFault;
        }
    } else {
        std::array<std::byte, 32> source1{};
        std::copy(state.xmm[vex.vvvv].begin(), state.xmm[vex.vvvv].end(),
                  source1.begin());
        if (vex.l)
            std::copy(state.ymm_high[vex.vvvv].begin(),
                      state.ymm_high[vex.vvvv].end(), source1.begin() + 16);
        std::array<std::byte, 32> result{};
        if (packed_compare) {
            const std::size_t lane_size =
                (opcode == 0x74 || opcode == 0x64) ? 1U :
                (opcode == 0x75 || opcode == 0x65) ? 2U : 4U;
            const bool equality =
                opcode == 0x74 || opcode == 0x75 || opcode == 0x76;
            for (std::size_t lane = 0; lane < width; lane += lane_size) {
                std::uint64_t lhs = 0;
                std::uint64_t rhs = 0;
                for (std::size_t byte = 0; byte < lane_size; ++byte) {
                    lhs |= static_cast<std::uint64_t>(
                        std::to_integer<unsigned char>(source1[lane + byte]))
                        << (byte * 8U);
                    rhs |= static_cast<std::uint64_t>(
                        std::to_integer<unsigned char>(source2[lane + byte]))
                        << (byte * 8U);
                }
                bool condition = false;
                if (equality) {
                    condition = lhs == rhs;
                } else if (lane_size == 1U) {
                    condition = std::bit_cast<std::int8_t>(
                        static_cast<std::uint8_t>(lhs)) >
                        std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rhs));
                } else if (lane_size == 2U) {
                    condition = std::bit_cast<std::int16_t>(
                        static_cast<std::uint16_t>(lhs)) >
                        std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs));
                } else {
                    condition = std::bit_cast<std::int32_t>(
                        static_cast<std::uint32_t>(lhs)) >
                        std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(rhs));
                }
                const std::uint64_t mask = lane_size == 4U
                    ? 0xffffffffULL : ((1ULL << (lane_size * 8U)) - 1ULL);
                const std::uint64_t value = condition ? mask : 0;
                for (std::size_t byte = 0; byte < lane_size; ++byte)
                    result[lane + byte] =
                        static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
            }
        } else if (is_and || is_andn || is_or || is_xor) {
            for (std::size_t i = 0; i < width; ++i) {
                if (is_and) result[i] = source1[i] & source2[i];
                else if (is_andn) result[i] = static_cast<std::byte>(
                    static_cast<unsigned char>(~std::to_integer<unsigned char>(source1[i])) &
                    std::to_integer<unsigned char>(source2[i]));
                else if (is_or) result[i] = source1[i] | source2[i];
                else result[i] = source1[i] ^ source2[i];
            }
        } else if (is_integer_arithmetic) {
            const std::size_t lane_size =
                (opcode == 0xfc || opcode == 0xf8 || opcode == 0xec ||
                 opcode == 0xdc || opcode == 0xe8 || opcode == 0xd8 ||
                 opcode == 0xda || opcode == 0xde || opcode == 0xe0) ? 1U :
                (opcode == 0xfd || opcode == 0xf9 || opcode == 0xd5 ||
                 opcode == 0xed || opcode == 0xdd || opcode == 0xe9 ||
                 opcode == 0xd9 || opcode == 0xea || opcode == 0xee ||
                 opcode == 0xe3) ? 2U :
                (opcode == 0xd4 || opcode == 0xfb) ? 8U : 4U;
            const bool add = opcode == 0xfc || opcode == 0xfd ||
                             opcode == 0xfe || opcode == 0xd4 ||
                             opcode == 0xec || opcode == 0xed;
            const bool signed_saturating =
                opcode == 0xec || opcode == 0xed || opcode == 0xe8 || opcode == 0xe9;
            const bool unsigned_saturating =
                opcode == 0xdc || opcode == 0xdd || opcode == 0xd8 || opcode == 0xd9;
            const bool is_minmax =
                opcode == 0xda || opcode == 0xde || opcode == 0xea || opcode == 0xee;
            const bool is_average = opcode == 0xe0 || opcode == 0xe3;
            for (std::size_t lane = 0; lane < width; lane += lane_size) {
                std::uint64_t lhs = 0;
                std::uint64_t rhs = 0;
                for (std::size_t byte = 0; byte < lane_size; ++byte) {
                    lhs |= static_cast<std::uint64_t>(
                        std::to_integer<unsigned char>(source1[lane + byte]))
                        << (byte * 8U);
                    rhs |= static_cast<std::uint64_t>(
                        std::to_integer<unsigned char>(source2[lane + byte]))
                        << (byte * 8U);
                }
                const std::uint64_t mask = lane_size == 8U
                    ? UINT64_MAX : ((1ULL << (lane_size * 8U)) - 1ULL);
                std::uint64_t value = 0;
                if (opcode == 0xd5) {
                    value = (lhs * rhs) & mask;
                } else if (is_average) {
                    value = (lhs + rhs + 1ULL) >> 1U;
                } else if (is_minmax) {
                    const bool minimum = opcode == 0xda || opcode == 0xea;
                    const bool signed_lanes = opcode == 0xea || opcode == 0xee;
                    if (signed_lanes) {
                        const std::int64_t signed_lhs = lane_size == 1U
                            ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(lhs))
                            : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lhs));
                        const std::int64_t signed_rhs = lane_size == 1U
                            ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rhs))
                            : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs));
                        value = static_cast<std::uint64_t>(
                            minimum ? std::min(signed_lhs, signed_rhs)
                                    : std::max(signed_lhs, signed_rhs)) & mask;
                    } else {
                        value = minimum ? std::min(lhs, rhs) : std::max(lhs, rhs);
                    }
                } else if (signed_saturating) {
                    const std::int64_t signed_lhs = lane_size == 1U
                        ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(lhs))
                        : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(lhs));
                    const std::int64_t signed_rhs = lane_size == 1U
                        ? std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rhs))
                        : std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs));
                    const std::int64_t minimum = lane_size == 1U ? -128 : -32768;
                    const std::int64_t maximum = lane_size == 1U ? 127 : 32767;
                    value = static_cast<std::uint64_t>(std::clamp(
                        add ? signed_lhs + signed_rhs : signed_lhs - signed_rhs,
                        minimum, maximum)) & mask;
                } else if (unsigned_saturating) {
                    const bool unsigned_add = opcode == 0xdc || opcode == 0xdd;
                    value = unsigned_add ? std::min(mask, lhs + rhs)
                                         : (lhs > rhs ? lhs - rhs : 0);
                } else {
                    value = (add ? lhs + rhs : lhs - rhs) & mask;
                }
                for (std::size_t byte = 0; byte < lane_size; ++byte)
                    result[lane + byte] =
                        static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
            }
        } else {
            const std::size_t lane_size = vex.pp == 1 ? 8U : 4U;
            for (std::size_t lane = 0; lane < width / lane_size; ++lane) {
                if (lane_size == 8U) {
                    std::uint64_t lhs_bits = 0;
                    std::uint64_t rhs_bits = 0;
                    for (std::size_t byte = 0; byte < 8; ++byte) {
                        lhs_bits |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(source1[lane * 8U + byte]))
                            << (byte * 8U);
                        rhs_bits |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(source2[lane * 8U + byte]))
                            << (byte * 8U);
                    }
                    const double lhs = std::bit_cast<double>(lhs_bits);
                    const double rhs = std::bit_cast<double>(rhs_bits);
                    double value = 0.0;
                    if (is_add) value = lhs + rhs;
                    else if (is_sub) value = lhs - rhs;
                    else if (is_mul) value = lhs * rhs;
                    else value = lhs / rhs;
                    const std::uint64_t bits = std::bit_cast<std::uint64_t>(value);
                    for (std::size_t byte = 0; byte < 8; ++byte)
                        result[lane * 8U + byte] =
                            static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
                } else {
                std::uint32_t lhs_bits = 0;
                std::uint32_t rhs_bits = 0;
                for (std::size_t byte = 0; byte < 4; ++byte) {
                    lhs_bits |= static_cast<std::uint32_t>(
                        std::to_integer<unsigned char>(source1[lane * 4U + byte]))
                        << (byte * 8U);
                    rhs_bits |= static_cast<std::uint32_t>(
                        std::to_integer<unsigned char>(source2[lane * 4U + byte]))
                        << (byte * 8U);
                }
                    const float lhs = std::bit_cast<float>(lhs_bits);
                    const float rhs = std::bit_cast<float>(rhs_bits);
                    float value = 0.0F;
                    if (is_add) value = lhs + rhs;
                    else if (is_sub) value = lhs - rhs;
                    else if (is_mul) value = lhs * rhs;
                    else value = lhs / rhs;
                    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
                    for (std::size_t byte = 0; byte < 4; ++byte)
                        result[lane * 4U + byte] =
                            static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
                }
            }
        }
        std::copy_n(result.begin(), 16, state.xmm[destination].begin());
        if (vex.l)
            std::copy_n(result.begin() + 16, 16, state.ymm_high[destination].begin());
        else
            state.ymm_high[destination].fill(std::byte{0});
    }
    state.rip = pc;
    return StepResult::Executed;
}

} // namespace

template <typename Memory>
StepResult step_impl(CpuState& state, Memory& memory) noexcept {
    std::uint64_t pc = state.rip;
    std::uint8_t opcode = 0;
    if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;

    if (opcode == 0xc5 || opcode == 0xc4) {
        VexPrefix vex{};
        if (opcode == 0xc5) {
            std::uint8_t second = 0;
            if (!fetch8(memory, pc, second)) return StepResult::MemoryFault;
            vex.r = (second & 0x80U) == 0;
            vex.map = 1;
            vex.vvvv = static_cast<std::uint8_t>((~(second >> 3U)) & 0x0fU);
            vex.l = (second & 0x04U) != 0;
            vex.pp = static_cast<std::uint8_t>(second & 0x03U);
        } else {
            std::uint8_t second = 0;
            std::uint8_t third = 0;
            if (!fetch8(memory, pc, second) || !fetch8(memory, pc, third))
                return StepResult::MemoryFault;
            vex.r = (second & 0x80U) == 0;
            vex.x = (second & 0x40U) == 0;
            vex.b = (second & 0x20U) == 0;
            vex.map = static_cast<std::uint8_t>(second & 0x1fU);
            vex.w = (third & 0x80U) != 0;
            vex.vvvv = static_cast<std::uint8_t>((~(third >> 3U)) & 0x0fU);
            vex.l = (third & 0x04U) != 0;
            vex.pp = static_cast<std::uint8_t>(third & 0x03U);
        }
        if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;
        return execute_vex(state, memory, pc, opcode, vex);
    }

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

    bool operand_size_override = false;
    bool prefix_f3 = false;
    bool prefix_f2 = false;
    while (opcode == 0x66 || opcode == 0xf3 || opcode == 0xf2) {
        if (opcode == 0x66) operand_size_override = true;
        else if (opcode == 0xf3) prefix_f3 = true;
        else prefix_f2 = true;
        if (!fetch8(memory, pc, opcode)) return StepResult::MemoryFault;
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

    // Indirect near CALL/JMP and PUSH through the FF group.
    if (opcode == 0xff) {
        if (operand_size_override || prefix_f3 || prefix_f2)
            return StepResult::UnsupportedInstruction;
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t operation =
            static_cast<std::uint8_t>((modrm >> 3U) & 7U);
        if (operation != 2U && operation != 4U && operation != 6U)
            return StepResult::UnsupportedInstruction;
        std::uint64_t target = 0;
        if ((modrm >> 6U) == 3U) {
            const std::size_t source =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            target = state.gpr[source];
        } else {
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, 0, address))
                return StepResult::MemoryFault;
            const auto value = memory.template read_le<std::uint64_t>(address);
            if (!value) return StepResult::MemoryFault;
            target = *value;
        }
        if (operation == 6U) {
            if (!push64(state, memory, target)) return StepResult::MemoryFault;
            state.rip = pc;
            return StepResult::Executed;
        }
        if (!canonical_address(target))
            return StepResult::GeneralProtectionFault;
        if (operation == 2U && !push64(state, memory, pc))
            return StepResult::MemoryFault;
        state.rip = target;
        return StepResult::Executed;
    }

    // SHL/SHR/SAR group-2 shifts with immediate, one, or CL counts.
    if (opcode == 0xc1 || opcode == 0xd1 || opcode == 0xd3) {
        if (prefix_f3 || prefix_f2) return StepResult::UnsupportedInstruction;
        const std::size_t width = rex_w ? 8U : (operand_size_override ? 2U : 4U);
        const unsigned bits = static_cast<unsigned>(width * 8U);
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::uint8_t operation =
            static_cast<std::uint8_t>((modrm >> 3U) & 7U);
        if (operation != 4U && operation != 5U && operation != 7U)
            return StepResult::UnsupportedInstruction;
        std::uint8_t raw_count = 1;
        if (opcode == 0xd3)
            raw_count = static_cast<std::uint8_t>(state.gpr[1]);
        const std::uint64_t mask = width == 8U ? UINT64_MAX :
            width == 4U ? 0xffffffffULL : 0xffffULL;
        std::uint64_t value = 0;
        std::uint64_t address = 0;
        const bool register_operand = (modrm >> 6U) == 3U;
        const std::size_t target_register =
            static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
        if (!register_operand &&
            !decode_effective_address(state, memory, pc, modrm,
                                      rex_b, rex_x, opcode == 0xc1 ? 1U : 0U,
                                      address))
            return StepResult::MemoryFault;
        if (opcode == 0xc1 && !fetch8(memory, pc, raw_count))
            return StepResult::MemoryFault;
        const unsigned count = raw_count & (width == 8U ? 0x3fU : 0x1fU);
        if (register_operand) {
            value = state.gpr[target_register] & mask;
        } else {
            if (width == 2U) {
                const auto loaded = memory.template read_le<std::uint16_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                value = *loaded;
            } else if (width == 4U) {
                const auto loaded = memory.template read_le<std::uint32_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                value = *loaded;
            } else {
                const auto loaded = memory.template read_le<std::uint64_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                value = *loaded;
            }
        }
        if (count == 0) {
            state.rip = pc;
            return StepResult::Executed;
        }
        const bool sign = ((value >> (bits - 1U)) & 1ULL) != 0;
        std::uint64_t result = 0;
        bool carry = false;
        if (operation == 4U) { // SHL/SAL
            if (count <= bits) carry = ((value >> (bits - count)) & 1ULL) != 0;
            if (count < bits) result = (value << count) & mask;
        } else if (operation == 5U) { // SHR
            if (count <= bits) carry = ((value >> (count - 1U)) & 1ULL) != 0;
            if (count < bits) result = value >> count;
        } else { // SAR
            carry = count <= bits
                ? ((value >> (count - 1U)) & 1ULL) != 0 : sign;
            if (count >= bits) {
                result = sign ? mask : 0;
            } else {
                result = value >> count;
                if (sign) result |= mask ^ (mask >> count);
            }
        }
        result &= mask;
        if (register_operand) {
            if (width == 8U) state.gpr[target_register] = result;
            else if (width == 4U)
                state.gpr[target_register] = static_cast<std::uint32_t>(result);
            else
                state.gpr[target_register] =
                    (state.gpr[target_register] & ~0xffffULL) | result;
        } else {
            bool written = false;
            if (width == 2U)
                written = memory.template write_le<std::uint16_t>(
                    address, static_cast<std::uint16_t>(result));
            else if (width == 4U)
                written = memory.template write_le<std::uint32_t>(
                    address, static_cast<std::uint32_t>(result));
            else
                written = memory.template write_le<std::uint64_t>(address, result);
            if (!written) return StepResult::MemoryFault;
        }
        state.rflags &= ~(flag_cf | flag_zf | flag_sf | flag_pf);
        if (carry) state.rflags |= flag_cf;
        if (result == 0) state.rflags |= flag_zf;
        if (((result >> (bits - 1U)) & 1ULL) != 0) state.rflags |= flag_sf;
        if ((std::popcount(static_cast<unsigned int>(result & 0xffU)) & 1) == 0)
            state.rflags |= flag_pf;
        if (count == 1U) {
            state.rflags &= ~flag_of;
            bool overflow = false;
            if (operation == 4U)
                overflow = (((result >> (bits - 1U)) & 1ULL) != 0) != carry;
            else if (operation == 5U)
                overflow = sign;
            if (overflow) state.rflags |= flag_of;
        }
        state.rip = pc;
        return StepResult::Executed;
    }

    // Two/three-operand IMUL with signed immediate operands.
    if (opcode == 0x69 || opcode == 0x6b) {
        if (prefix_f3 || prefix_f2) return StepResult::UnsupportedInstruction;
        const std::size_t width = rex_w ? 8U : (operand_size_override ? 2U : 4U);
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::size_t destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
        std::uint64_t source = 0;
        std::uint64_t address = 0;
        if ((modrm >> 6U) == 3U) {
            const std::size_t source_register =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            source = state.gpr[source_register];
        } else {
            const std::size_t immediate_size = opcode == 0x6b ? 1U :
                                               (width == 2U ? 2U : 4U);
            if (!decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, immediate_size, address))
                return StepResult::MemoryFault;
            if (width == 2U) {
                const auto value = memory.template read_le<std::uint16_t>(address);
                if (!value) return StepResult::MemoryFault;
                source = *value;
            } else if (width == 4U) {
                const auto value = memory.template read_le<std::uint32_t>(address);
                if (!value) return StepResult::MemoryFault;
                source = *value;
            } else {
                const auto value = memory.template read_le<std::uint64_t>(address);
                if (!value) return StepResult::MemoryFault;
                source = *value;
            }
        }
        std::uint64_t immediate = 0;
        if (opcode == 0x6b) {
            std::uint8_t raw = 0;
            if (!fetch8(memory, pc, raw)) return StepResult::MemoryFault;
            immediate = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(std::bit_cast<std::int8_t>(raw)));
        } else if (width == 2U) {
            std::uint8_t low = 0;
            std::uint8_t high = 0;
            if (!fetch8(memory, pc, low) || !fetch8(memory, pc, high))
                return StepResult::MemoryFault;
            const std::uint16_t raw = static_cast<std::uint16_t>(low) |
                static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U);
            immediate = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(std::bit_cast<std::int16_t>(raw)));
        } else {
            std::uint32_t raw = 0;
            for (std::size_t byte = 0; byte < 4; ++byte) {
                std::uint8_t part = 0;
                if (!fetch8(memory, pc, part)) return StepResult::MemoryFault;
                raw |= static_cast<std::uint32_t>(part) << (byte * 8U);
            }
            immediate = static_cast<std::uint64_t>(
                static_cast<std::int64_t>(std::bit_cast<std::int32_t>(raw)));
        }
        const std::uint64_t mask = width == 8U ? UINT64_MAX :
                                   width == 4U ? 0xffffffffULL : 0xffffULL;
        const auto product = signed_multiply(source & mask, immediate & mask, width);
        if (width == 8U) state.gpr[destination] = product.value;
        else if (width == 4U)
            state.gpr[destination] = static_cast<std::uint32_t>(product.value);
        else
            state.gpr[destination] =
                (state.gpr[destination] & ~0xffffULL) | (product.value & 0xffffULL);
        if (product.overflow) state.rflags |= flag_cf | flag_of;
        else state.rflags &= ~(flag_cf | flag_of);
        state.rip = pc;
        return StepResult::Executed;
    }

    // LEA computes an effective address without reading guest memory.
    if (opcode == 0x8d) {
        if (operand_size_override) return StepResult::UnsupportedInstruction;
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        if ((modrm >> 6U) == 3U) return StepResult::UnsupportedInstruction;
        const std::size_t destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
        std::uint64_t address = 0;
        if (!decode_effective_address(state, memory, pc, modrm,
                                      rex_b, rex_x, 0, address))
            return StepResult::MemoryFault;
        state.gpr[destination] = rex_w
            ? address : static_cast<std::uint32_t>(address);
        state.rip = pc;
        return StepResult::Executed;
    }

    // MOVSXD r64, r/m32.
    if (opcode == 0x63) {
        if (!rex_w || operand_size_override)
            return StepResult::UnsupportedInstruction;
        std::uint8_t modrm = 0;
        if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
        const std::size_t destination =
            static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
        std::uint32_t source = 0;
        if ((modrm >> 6U) == 3U) {
            const std::size_t source_register =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            source = static_cast<std::uint32_t>(state.gpr[source_register]);
        } else {
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, 0, address))
                return StepResult::MemoryFault;
            const auto value = memory.template read_le<std::uint32_t>(address);
            if (!value) return StepResult::MemoryFault;
            source = *value;
        }
        state.gpr[destination] = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::bit_cast<std::int32_t>(source)));
        state.rip = pc;
        return StepResult::Executed;
    }

    // Near relative JZ/JNZ (0F 84 / 0F 85), with a signed 32-bit offset.
    if (opcode == 0x0f) {
        std::uint8_t second = 0;
        if (!fetch8(memory, pc, second)) return StepResult::MemoryFault;

        // CMOVcc r16/32/64, r/m16/32/64.
        if (second >= 0x40 && second <= 0x4f) {
            if (prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            const std::size_t width = rex_w ? 8U : (operand_size_override ? 2U : 4U);
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            std::uint64_t source = 0;
            if ((modrm >> 6U) == 3U) {
                const std::size_t source_register =
                    static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
                source = state.gpr[source_register];
            } else {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                if (width == 2U) {
                    const auto value = memory.template read_le<std::uint16_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                } else if (width == 4U) {
                    const auto value = memory.template read_le<std::uint32_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                } else {
                    const auto value = memory.template read_le<std::uint64_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                }
            }
            if (condition_true(static_cast<std::uint8_t>(second & 0x0fU),
                               state.rflags)) {
                if (width == 8U) state.gpr[destination] = source;
                else if (width == 4U)
                    state.gpr[destination] = static_cast<std::uint32_t>(source);
                else
                    state.gpr[destination] =
                        (state.gpr[destination] & ~0xffffULL) | (source & 0xffffULL);
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // SETcc r/m8.
        if (second >= 0x90 && second <= 0x9f) {
            if (prefix_f3 || prefix_f2 || operand_size_override)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if (((modrm >> 3U) & 7U) != 0U)
                return StepResult::UnsupportedInstruction;
            const std::uint8_t value = condition_true(
                static_cast<std::uint8_t>(second & 0x0fU), state.rflags) ? 1U : 0U;
            if ((modrm >> 6U) == 3U) {
                const std::size_t target =
                    static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
                if (!rex_present && target >= 4U && target <= 7U) {
                    const std::size_t base = target - 4U;
                    state.gpr[base] = (state.gpr[base] & ~0xff00ULL) |
                                      (static_cast<std::uint64_t>(value) << 8U);
                } else {
                    state.gpr[target] = (state.gpr[target] & ~0xffULL) | value;
                }
            } else {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                const std::array<std::byte, 1> byte{static_cast<std::byte>(value)};
                if (!memory.write(address, byte)) return StepResult::MemoryFault;
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // MOVZX/MOVSX byte and word sources into 16/32/64-bit GPR destinations.
        if (second == 0xb6 || second == 0xb7 ||
            second == 0xbe || second == 0xbf) {
            if (prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const bool byte_source = second == 0xb6 || second == 0xbe;
            const bool sign_extend = second == 0xbe || second == 0xbf;
            const std::size_t source_width = byte_source ? 1U : 2U;
            std::uint64_t source = 0;
            if ((modrm >> 6U) == 3U) {
                const std::size_t source_register =
                    static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
                if (byte_source && !rex_present &&
                    source_register >= 4U && source_register <= 7U) {
                    source = (state.gpr[source_register - 4U] >> 8U) & 0xffU;
                } else {
                    source = state.gpr[source_register] &
                        (byte_source ? 0xffU : 0xffffU);
                }
            } else {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                if (byte_source) {
                    const auto value = memory.template read_le<std::uint8_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                } else {
                    const auto value = memory.template read_le<std::uint16_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                }
            }
            std::uint64_t value = source;
            if (sign_extend) {
                const std::int64_t signed_value = byte_source
                    ? static_cast<std::int8_t>(source)
                    : static_cast<std::int16_t>(source);
                value = static_cast<std::uint64_t>(signed_value);
            }
            if (rex_w) {
                state.gpr[destination] = value;
            } else if (operand_size_override) {
                state.gpr[destination] =
                    (state.gpr[destination] & ~0xffffULL) | (value & 0xffffULL);
            } else {
                state.gpr[destination] = static_cast<std::uint32_t>(value);
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // Two-operand signed IMUL r16/32/64, r/m16/32/64.
        if (second == 0xaf) {
            if (prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            const std::size_t width = rex_w ? 8U : (operand_size_override ? 2U : 4U);
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            std::uint64_t source = 0;
            if ((modrm >> 6U) == 3U) {
                const std::size_t source_register =
                    static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
                source = state.gpr[source_register];
            } else {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                if (width == 2U) {
                    const auto value = memory.template read_le<std::uint16_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                } else if (width == 4U) {
                    const auto value = memory.template read_le<std::uint32_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                } else {
                    const auto value = memory.template read_le<std::uint64_t>(address);
                    if (!value) return StepResult::MemoryFault;
                    source = *value;
                }
            }
            const std::uint64_t mask = width == 8U ? UINT64_MAX :
                                       width == 4U ? 0xffffffffULL : 0xffffULL;
            const auto product = signed_multiply(state.gpr[destination] & mask,
                                                 source & mask, width);
            if (width == 8U) state.gpr[destination] = product.value;
            else if (width == 4U)
                state.gpr[destination] = static_cast<std::uint32_t>(product.value);
            else
                state.gpr[destination] =
                    (state.gpr[destination] & ~0xffffULL) | (product.value & 0xffffULL);
            if (product.overflow) state.rflags |= flag_cf | flag_of;
            else state.rflags &= ~(flag_cf | flag_of);
            state.rip = pc;
            return StepResult::Executed;
        }

        // 128-bit packed and scalar SSE register/memory moves.
        if (second == 0x10 || second == 0x11 ||
            second == 0x28 || second == 0x29) {
            const bool load = second == 0x10 || second == 0x28;
            const bool aligned = second == 0x28 || second == 0x29;
            const bool no_prefix = !operand_size_override && !prefix_f3 && !prefix_f2;
            const bool packed_double = operand_size_override && !prefix_f3 && !prefix_f2;
            const bool scalar_single = prefix_f3 && !operand_size_override && !prefix_f2;
            const bool scalar_double = prefix_f2 && !operand_size_override && !prefix_f3;
            const bool scalar = scalar_single || scalar_double;
            const bool valid = aligned ? (no_prefix || packed_double)
                                       : (no_prefix || packed_double ||
                                          scalar_single || scalar_double);
            if (!valid) return StepResult::UnsupportedInstruction;
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;

            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t xmm_reg =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t xmm_rm =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            const bool register_operand = (modrm >> 6U) == 3U;
            const std::size_t width = scalar_single ? 4U :
                                      scalar_double ? 8U : 16U;
            std::uint64_t address = 0;
            std::array<std::byte, 16> source{};
            if (register_operand) {
                source = state.xmm[xmm_rm];
            } else {
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 0, address))
                    return StepResult::MemoryFault;
                if (aligned && (address & 0xfU) != 0)
                    return StepResult::GeneralProtectionFault;
                if (load && !memory.read(address,
                        std::span<std::byte>{source}.first(width)))
                    return StepResult::MemoryFault;
            }

            if (load) {
                if (scalar) {
                    if (!register_operand) state.xmm[xmm_reg].fill(std::byte{0});
                    std::copy_n(source.begin(), width, state.xmm[xmm_reg].begin());
                } else {
                    state.xmm[xmm_reg] = source;
                }
            } else if (register_operand) {
                if (scalar)
                    std::copy_n(state.xmm[xmm_reg].begin(), width,
                                state.xmm[xmm_rm].begin());
                else
                    state.xmm[xmm_rm] = state.xmm[xmm_reg];
            } else if (!memory.write(address,
                       std::span<const std::byte>{state.xmm[xmm_reg]}.first(width))) {
                return StepResult::MemoryFault;
            }
            state.rip = pc;
            return StepResult::Executed;
        }


        // Legacy SSE CMPPS/CMPPD/CMPSS/CMPSD with the eight imm8 predicates.
        if (second == 0xc2) {
            const bool scalar = prefix_f3 || prefix_f2;
            const bool double_precision = operand_size_override || prefix_f2;
            const bool valid_prefix =
                (!operand_size_override && !prefix_f3 && !prefix_f2) ||
                (operand_size_override && !prefix_f3 && !prefix_f2) ||
                (!operand_size_override && prefix_f3 && !prefix_f2) ||
                (!operand_size_override && !prefix_f3 && prefix_f2);
            if (!valid_prefix) return StepResult::UnsupportedInstruction;
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t source_reg =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            const bool register_operand = (modrm >> 6U) == 3U;
            const std::size_t lane_size = double_precision ? 8U : 4U;
            const std::size_t source_size = scalar ? lane_size : 16U;
            std::array<std::byte, 16> source{};
            if (register_operand) {
                source = state.xmm[source_reg];
            } else {
                std::uint64_t address = 0;
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, 1, address))
                    return StepResult::MemoryFault;
                if (!memory.read(address,
                        std::span<std::byte>{source}.first(source_size)))
                    return StepResult::MemoryFault;
            }
            std::uint8_t predicate = 0;
            if (!fetch8(memory, pc, predicate)) return StepResult::MemoryFault;
            predicate &= 7U;
            const std::size_t lane_count = scalar ? 1U : 16U / lane_size;
            for (std::size_t lane = 0; lane < lane_count; ++lane) {
                bool unordered = false;
                bool equal = false;
                bool less = false;
                bool less_equal = false;
                if (double_precision) {
                    std::uint64_t lhs_bits = 0;
                    std::uint64_t rhs_bits = 0;
                    for (std::size_t byte = 0; byte < 8; ++byte) {
                        lhs_bits |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(
                                state.xmm[destination][lane * 8U + byte])) << (byte * 8U);
                        rhs_bits |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(
                                source[lane * 8U + byte])) << (byte * 8U);
                    }
                    const double lhs = std::bit_cast<double>(lhs_bits);
                    const double rhs = std::bit_cast<double>(rhs_bits);
                    unordered = std::isnan(lhs) || std::isnan(rhs);
                    equal = lhs == rhs;
                    less = lhs < rhs;
                    less_equal = lhs <= rhs;
                } else {
                    std::uint32_t lhs_bits = 0;
                    std::uint32_t rhs_bits = 0;
                    for (std::size_t byte = 0; byte < 4; ++byte) {
                        lhs_bits |= static_cast<std::uint32_t>(
                            std::to_integer<unsigned char>(
                                state.xmm[destination][lane * 4U + byte])) << (byte * 8U);
                        rhs_bits |= static_cast<std::uint32_t>(
                            std::to_integer<unsigned char>(
                                source[lane * 4U + byte])) << (byte * 8U);
                    }
                    const float lhs = std::bit_cast<float>(lhs_bits);
                    const float rhs = std::bit_cast<float>(rhs_bits);
                    unordered = std::isnan(lhs) || std::isnan(rhs);
                    equal = lhs == rhs;
                    less = lhs < rhs;
                    less_equal = lhs <= rhs;
                }
                bool result = false;
                switch (predicate) {
                case 0: result = !unordered && equal; break;       // EQ_OQ
                case 1: result = !unordered && less; break;        // LT_OS
                case 2: result = !unordered && less_equal; break;  // LE_OS
                case 3: result = unordered; break;                 // UNORD_Q
                case 4: result = unordered || !equal; break;       // NEQ_UQ
                case 5: result = unordered || !less; break;        // NLT_US
                case 6: result = unordered || !less_equal; break;  // NLE_US
                case 7: result = !unordered; break;                // ORD_Q
                }
                const std::uint64_t mask = double_precision
                    ? UINT64_MAX : 0xffffffffULL;
                const std::uint64_t value = result ? mask : 0;
                for (std::size_t byte = 0; byte < lane_size; ++byte)
                    state.xmm[destination][lane * lane_size + byte] =
                        static_cast<std::byte>((value >> (byte * 8U)) & 0xffU);
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // Initial 128-bit SSE/SSE2 data movement and logical operations.
        const bool packed_add_sub =
            second == 0xfc || second == 0xfd || second == 0xfe || second == 0xd4 ||
            second == 0xf8 || second == 0xf9 || second == 0xfa || second == 0xfb;
        const bool packed_compare =
            second == 0x74 || second == 0x75 || second == 0x76 ||
            second == 0x64 || second == 0x65 || second == 0x66;
        const bool shuffle_operation = second == 0x70 || second == 0xc6;
        const bool fp_bitwise = second == 0x54 || second == 0x55 ||
                                second == 0x56 || second == 0x57;
        const bool floating_arithmetic =
            second == 0x58 || second == 0x5c || second == 0x59 || second == 0x5e ||
            second == 0x51 || second == 0x5d || second == 0x5f;
        const bool shuffle_pshufd = second == 0x70 && operand_size_override &&
                                    !prefix_f3 && !prefix_f2;
        const bool shuffle_shufps = second == 0xc6 && !operand_size_override &&
                                    !prefix_f3 && !prefix_f2;

        if (second == 0x50) { // MOVMSKPS/MOVMSKPD r32, xmm
            if (prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if ((modrm >> 6U) != 3U) return StepResult::UnsupportedInstruction;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t source =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            const std::size_t lane_size = operand_size_override ? 8U : 4U;
            std::uint32_t mask = 0;
            for (std::size_t lane = 0; lane < 16U / lane_size; ++lane) {
                const std::size_t sign_byte = lane * lane_size + lane_size - 1U;
                if ((std::to_integer<unsigned char>(state.xmm[source][sign_byte]) &
                     0x80U) != 0)
                    mask |= 1U << lane;
            }
            state.gpr[destination] = mask;
            state.rip = pc;
            return StepResult::Executed;
        }
        if (second == 0xd7) { // PMOVMSKB r32, xmm
            if (!operand_size_override || prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            if ((modrm >> 6U) != 3U) return StepResult::UnsupportedInstruction;
            const std::size_t destination =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t source =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            std::uint32_t mask = 0;
            for (std::size_t i = 0; i < 16; ++i) {
                if ((std::to_integer<unsigned char>(state.xmm[source][i]) & 0x80U) != 0)
                    mask |= 1U << i;
            }
            state.gpr[destination] = mask;
            state.rip = pc;
            return StepResult::Executed;
        }
        if (second == 0x6f || second == 0x7f || second == 0xef ||
            second == 0xdb || second == 0xeb || second == 0x57 || fp_bitwise ||
            packed_add_sub || packed_compare || shuffle_operation ||
            floating_arithmetic) {
            const bool is_move = second == 0x6f || second == 0x7f;
            const bool aligned_move = is_move && operand_size_override && !prefix_f3;
            const bool unaligned_move = is_move && prefix_f3 && !operand_size_override;
            const bool bitwise_legacy = !is_move && fp_bitwise &&
                                        !prefix_f3 && !prefix_f2;
            const bool bitwise_sse2 = !is_move && !packed_add_sub && second != 0x57 &&
                                      operand_size_override && !prefix_f3 && !prefix_f2;
            const bool packed_sse2 = (packed_add_sub || packed_compare) &&
                                     operand_size_override && !prefix_f3 && !prefix_f2;
            const bool valid_shuffle = shuffle_pshufd || shuffle_shufps;
            const bool valid_float = floating_arithmetic &&
                ((!operand_size_override && !prefix_f3 && !prefix_f2) ||
                 (operand_size_override && !prefix_f3 && !prefix_f2) ||
                 (!operand_size_override && prefix_f3 && !prefix_f2) ||
                 (!operand_size_override && !prefix_f3 && prefix_f2));
            if (!(aligned_move || unaligned_move || bitwise_legacy ||
                  bitwise_sse2 || packed_sse2 || valid_shuffle || valid_float) || prefix_f2 && !valid_float)
                return StepResult::UnsupportedInstruction;

            // This first cut requires OSFXSR and rejects CR0.EM/TS rather than
            // pretending to implement the corresponding #UD/#NM exception path.
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;

            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t reg_index =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t rm_index =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            const bool register_operand = (modrm >> 6U) == 3U;
            std::uint64_t address = 0;
            std::array<std::byte, 16> source{};
            if (register_operand) {
                source = state.xmm[rm_index];
            } else {
                if (!decode_effective_address(state, memory, pc, modrm,
                                              rex_b, rex_x, shuffle_operation ? 1U : 0U,
                                              address))
                    return StepResult::MemoryFault;
                if (aligned_move && (address & 0xfU) != 0)
                    return StepResult::GeneralProtectionFault;
                if (second == 0x6f || second == 0xef ||
                    second == 0xdb || second == 0xeb || second == 0x57 ||
                    shuffle_operation || floating_arithmetic) {
                    const bool scalar_float = floating_arithmetic &&
                                              (prefix_f3 || prefix_f2);
                    const std::size_t read_size = scalar_float
                        ? (prefix_f2 ? 8U : 4U) : 16U;
                    if (!memory.read(address,
                            std::span<std::byte>{source}.first(read_size)))
                        return StepResult::MemoryFault;
                }
            }

            if (shuffle_operation) {
                std::uint8_t immediate = 0;
                if (!fetch8(memory, pc, immediate)) return StepResult::MemoryFault;
                const auto old_destination = state.xmm[reg_index];
                const auto read_dword = [](const std::array<std::byte, 16>& value,
                                           std::size_t lane) {
                    std::uint32_t result = 0;
                    for (std::size_t i = 0; i < 4; ++i)
                        result |= static_cast<std::uint32_t>(
                            std::to_integer<unsigned char>(value[lane * 4U + i]))
                            << (i * 8U);
                    return result;
                };
                const auto write_dword = [](std::array<std::byte, 16>& value,
                                            std::size_t lane, std::uint32_t dword) {
                    for (std::size_t i = 0; i < 4; ++i)
                        value[lane * 4U + i] =
                            static_cast<std::byte>((dword >> (i * 8U)) & 0xffU);
                };
                if (shuffle_pshufd) {
                    for (std::size_t lane = 0; lane < 4; ++lane) {
                        const std::size_t selected =
                            (immediate >> (lane * 2U)) & 3U;
                        write_dword(state.xmm[reg_index], lane,
                                    read_dword(source, selected));
                    }
                } else {
                    write_dword(state.xmm[reg_index], 0,
                                read_dword(old_destination, immediate & 3U));
                    write_dword(state.xmm[reg_index], 1,
                                read_dword(old_destination, (immediate >> 2U) & 3U));
                    write_dword(state.xmm[reg_index], 2,
                                read_dword(source, (immediate >> 4U) & 3U));
                    write_dword(state.xmm[reg_index], 3,
                                read_dword(source, (immediate >> 6U) & 3U));
                }
            } else if (second == 0x6f) {
                state.xmm[reg_index] = source;
            } else if (second == 0x7f) {
                if (register_operand) state.xmm[rm_index] = state.xmm[reg_index];
                else if (!memory.write(address, state.xmm[reg_index]))
                    return StepResult::MemoryFault;
            } else if (floating_arithmetic) {
                const bool double_precision = operand_size_override || prefix_f2;
                const bool scalar = prefix_f3 || prefix_f2;
                if (double_precision) {
                    const std::size_t lane_count = scalar ? 1U : 2U;
                    for (std::size_t lane = 0; lane < lane_count; ++lane) {
                        std::uint64_t lhs_bits = 0;
                        std::uint64_t rhs_bits = 0;
                        for (std::size_t byte = 0; byte < 8; ++byte) {
                            lhs_bits |= static_cast<std::uint64_t>(
                                std::to_integer<unsigned char>(
                                    state.xmm[reg_index][lane * 8U + byte])) << (byte * 8U);
                            rhs_bits |= static_cast<std::uint64_t>(
                                std::to_integer<unsigned char>(
                                    source[lane * 8U + byte])) << (byte * 8U);
                        }
                        const double lhs = std::bit_cast<double>(lhs_bits);
                        const double rhs = std::bit_cast<double>(rhs_bits);
                        double result = 0.0;
                        if (second == 0x51) result = std::sqrt(rhs);
                        else if (second == 0x5d) result = lhs < rhs ? lhs : rhs;
                        else if (second == 0x5f) result = lhs > rhs ? lhs : rhs;
                        else if (second == 0x58) result = lhs + rhs;
                        else if (second == 0x5c) result = lhs - rhs;
                        else if (second == 0x59) result = lhs * rhs;
                        else result = lhs / rhs;
                        const std::uint64_t result_bits = std::bit_cast<std::uint64_t>(result);
                        for (std::size_t byte = 0; byte < 8; ++byte)
                            state.xmm[reg_index][lane * 8U + byte] =
                                static_cast<std::byte>((result_bits >> (byte * 8U)) & 0xffU);
                    }
                } else {
                    const std::size_t lane_count = scalar ? 1U : 4U;
                    for (std::size_t lane = 0; lane < lane_count; ++lane) {
                        std::uint32_t lhs_bits = 0;
                        std::uint32_t rhs_bits = 0;
                        for (std::size_t byte = 0; byte < 4; ++byte) {
                            lhs_bits |= static_cast<std::uint32_t>(
                                std::to_integer<unsigned char>(
                                    state.xmm[reg_index][lane * 4U + byte])) << (byte * 8U);
                            rhs_bits |= static_cast<std::uint32_t>(
                                std::to_integer<unsigned char>(
                                    source[lane * 4U + byte])) << (byte * 8U);
                        }
                        const float lhs = std::bit_cast<float>(lhs_bits);
                        const float rhs = std::bit_cast<float>(rhs_bits);
                        float result = 0.0F;
                        if (second == 0x51) result = std::sqrt(rhs);
                        else if (second == 0x5d) result = lhs < rhs ? lhs : rhs;
                        else if (second == 0x5f) result = lhs > rhs ? lhs : rhs;
                        else if (second == 0x58) result = lhs + rhs;
                        else if (second == 0x5c) result = lhs - rhs;
                        else if (second == 0x59) result = lhs * rhs;
                        else result = lhs / rhs;
                        const std::uint32_t result_bits = std::bit_cast<std::uint32_t>(result);
                        for (std::size_t byte = 0; byte < 4; ++byte)
                            state.xmm[reg_index][lane * 4U + byte] =
                                static_cast<std::byte>((result_bits >> (byte * 8U)) & 0xffU);
                    }
                }
            } else if (packed_add_sub || packed_compare) {
                const std::size_t lane_size = packed_compare
                    ? ((second == 0x74 || second == 0x64) ? 1U :
                       (second == 0x75 || second == 0x65) ? 2U : 4U)
                    : ((second == 0xfc || second == 0xf8) ? 1U :
                       (second == 0xfd || second == 0xf9) ? 2U :
                       (second == 0xfe || second == 0xfa) ? 4U : 8U);
                const bool is_add =
                    second == 0xfc || second == 0xfd ||
                    second == 0xfe || second == 0xd4;
                const bool is_equal_compare =
                    second == 0x74 || second == 0x75 || second == 0x76;
                for (std::size_t lane = 0; lane < 16; lane += lane_size) {
                    std::uint64_t lhs = 0;
                    std::uint64_t rhs = 0;
                    for (std::size_t byte = 0; byte < lane_size; ++byte) {
                        lhs |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(state.xmm[reg_index][lane + byte]))
                            << (byte * 8U);
                        rhs |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(source[lane + byte]))
                            << (byte * 8U);
                    }
                    const std::uint64_t mask = lane_size == 8U
                        ? UINT64_MAX : ((1ULL << (lane_size * 8U)) - 1ULL);
                    std::uint64_t result = 0;
                    if (packed_add_sub) {
                        result = (is_add ? lhs + rhs : lhs - rhs) & mask;
                    } else {
                        bool condition = false;
                        if (is_equal_compare) {
                            condition = lhs == rhs;
                        } else if (lane_size == 1U) {
                            condition = std::bit_cast<std::int8_t>(
                                static_cast<std::uint8_t>(lhs)) >
                                std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(rhs));
                        } else if (lane_size == 2U) {
                            condition = std::bit_cast<std::int16_t>(
                                static_cast<std::uint16_t>(lhs)) >
                                std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(rhs));
                        } else {
                            condition = std::bit_cast<std::int32_t>(
                                static_cast<std::uint32_t>(lhs)) >
                                std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(rhs));
                        }
                        result = condition ? mask : 0;
                    }
                    for (std::size_t byte = 0; byte < lane_size; ++byte)
                        state.xmm[reg_index][lane + byte] =
                            static_cast<std::byte>((result >> (byte * 8U)) & 0xffU);
                }
            } else {
                for (std::size_t i = 0; i < source.size(); ++i) {
                    const auto lhs = std::to_integer<unsigned char>(state.xmm[reg_index][i]);
                    const auto rhs = std::to_integer<unsigned char>(source[i]);
                    unsigned char result = 0;
                    if (second == 0xef || second == 0x57) result = lhs ^ rhs;
                    else if (second == 0x55) result = static_cast<unsigned char>((~lhs) & rhs);
                    else if (second == 0xdb || second == 0x54) result = lhs & rhs;
                    else result = lhs | rhs;
                    state.xmm[reg_index][i] = static_cast<std::byte>(result);
                }
            }
            state.rip = pc;
            return StepResult::Executed;
        }



        // MOVD/MOVQ between a GPR or memory operand and an XMM register.
        if (second == 0x6e || second == 0x7e) {
            if (!operand_size_override || prefix_f3 || prefix_f2)
                return StepResult::UnsupportedInstruction;
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::size_t xmm_index =
                static_cast<std::size_t>((modrm >> 3U) & 7U) + (rex_r ? 8U : 0U);
            const std::size_t gpr_index =
                static_cast<std::size_t>(modrm & 7U) + (rex_b ? 8U : 0U);
            const std::size_t width = rex_w ? 8U : 4U;
            const bool register_operand = (modrm >> 6U) == 3U;
            std::uint64_t address = 0;
            if (!register_operand &&
                !decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, 0, address))
                return StepResult::MemoryFault;

            if (second == 0x6e) { // MOVD/MOVQ r/m, XMM
                std::array<std::byte, 8> value{};
                if (register_operand) {
                    for (std::size_t i = 0; i < width; ++i)
                        value[i] = static_cast<std::byte>(
                            (state.gpr[gpr_index] >> (i * 8U)) & 0xffU);
                } else if (!memory.read(address,
                            std::span<std::byte>{value}.first(width))) {
                    return StepResult::MemoryFault;
                }
                state.xmm[xmm_index].fill(std::byte{0});
                std::copy_n(value.begin(), width, state.xmm[xmm_index].begin());
            } else { // MOVD/MOVQ XMM, r/m
                if (register_operand) {
                    std::uint64_t value = 0;
                    for (std::size_t i = 0; i < width; ++i)
                        value |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(state.xmm[xmm_index][i]))
                            << (i * 8U);
                    state.gpr[gpr_index] = value;
                } else if (!memory.write(address,
                            std::span<const std::byte>{state.xmm[xmm_index]}.first(width))) {
                    return StepResult::MemoryFault;
                }
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // FXSAVE/FXRSTOR 512-byte state image (64-bit mode layout).
        if (second == 0xae) {
            if ((state.control_registers[0] & ((1ULL << 2U) | (1ULL << 3U))) != 0 ||
                (state.control_registers[4] & (1ULL << 9U)) == 0)
                return StepResult::UnsupportedInstruction;
            std::uint8_t modrm = 0;
            if (!fetch8(memory, pc, modrm)) return StepResult::MemoryFault;
            const std::uint8_t operation =
                static_cast<std::uint8_t>((modrm >> 3U) & 7U);
            if ((modrm >> 6U) == 3U)
                return StepResult::UnsupportedInstruction;
            std::uint64_t address = 0;
            if (!decode_effective_address(state, memory, pc, modrm,
                                          rex_b, rex_x, 0, address))
                return StepResult::MemoryFault;
            if (operation == 2U) { // LDMXCSR m32
                const auto value = memory.template read_le<std::uint32_t>(address);
                if (!value) return StepResult::MemoryFault;
                if ((*value & ~state.mxcsr_mask) != 0)
                    return StepResult::GeneralProtectionFault;
                state.mxcsr = *value;
                state.rip = pc;
                return StepResult::Executed;
            }
            if (operation == 3U) { // STMXCSR m32
                if (!memory.template write_le<std::uint32_t>(address, state.mxcsr))
                    return StepResult::MemoryFault;
                state.rip = pc;
                return StepResult::Executed;
            }
            if (operation == 4U || operation == 5U) { // XSAVE / XRSTOR
                if ((state.control_registers[4] & (1ULL << 18U)) == 0)
                    return StepResult::UnsupportedInstruction;
                if ((address & 0x3fU) != 0)
                    return StepResult::GeneralProtectionFault;
                const std::uint64_t request_mask =
                    (static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(state.gpr[2])) << 32U) |
                    static_cast<std::uint32_t>(state.gpr[0]);
                if (request_mask != state.xcr0)
                    return StepResult::UnsupportedInstruction;
                const std::size_t area_size =
                    (state.xcr0 & 4ULL) != 0 ? 832U : 576U;
                std::array<std::byte, 832> area{};
                const auto put16 = [&](std::size_t offset, std::uint16_t value) {
                    for (std::size_t i = 0; i < 2; ++i)
                        area[offset + i] =
                            static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
                };
                const auto put32 = [&](std::size_t offset, std::uint32_t value) {
                    for (std::size_t i = 0; i < 4; ++i)
                        area[offset + i] =
                            static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
                };
                const auto put64 = [&](std::size_t offset, std::uint64_t value) {
                    for (std::size_t i = 0; i < 8; ++i)
                        area[offset + i] =
                            static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
                };
                const auto get16 = [&](std::size_t offset) {
                    std::uint16_t value = 0;
                    for (std::size_t i = 0; i < 2; ++i)
                        value |= static_cast<std::uint16_t>(
                            std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                    return value;
                };
                const auto get32 = [&](std::size_t offset) {
                    std::uint32_t value = 0;
                    for (std::size_t i = 0; i < 4; ++i)
                        value |= static_cast<std::uint32_t>(
                            std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                    return value;
                };
                const auto get64 = [&](std::size_t offset) {
                    std::uint64_t value = 0;
                    for (std::size_t i = 0; i < 8; ++i)
                        value |= static_cast<std::uint64_t>(
                            std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                    return value;
                };

                if (operation == 4U) { // XSAVE
                    put16(0, state.x87_control_word);
                    put16(2, state.x87_status_word);
                    area[4] = static_cast<std::byte>(state.x87_tag_word);
                    put16(6, state.x87_opcode);
                    put64(8, state.x87_instruction_pointer);
                    put64(16, state.x87_data_pointer);
                    if ((state.xcr0 & 2ULL) != 0) {
                        put32(24, state.mxcsr);
                        put32(28, state.mxcsr_mask);
                    }
                    for (std::size_t i = 0; i < state.st.size(); ++i)
                        std::copy(state.st[i].begin(), state.st[i].end(),
                                  area.begin() + 32U + i * 16U);
                    if ((state.xcr0 & 2ULL) != 0) {
                        for (std::size_t i = 0; i < state.xmm.size(); ++i)
                            std::copy(state.xmm[i].begin(), state.xmm[i].end(),
                                      area.begin() + 160U + i * 16U);
                    }
                    put64(512, state.xcr0);
                    put64(520, 0); // Standard (non-compacted) XSAVE format.
                    if ((state.xcr0 & 4ULL) != 0) {
                        for (std::size_t i = 0; i < state.ymm_high.size(); ++i)
                            std::copy(state.ymm_high[i].begin(), state.ymm_high[i].end(),
                                      area.begin() + 576U + i * 16U);
                    }
                    if (!memory.write(address,
                            std::span<const std::byte>{area}.first(area_size)))
                        return StepResult::MemoryFault;
                } else { // XRSTOR
                    if (!memory.read(address,
                            std::span<std::byte>{area}.first(area_size)))
                        return StepResult::MemoryFault;
                    const std::uint64_t xstate_bv = get64(512);
                    const std::uint64_t xcomp_bv = get64(520);
                    if (xcomp_bv != 0 || (xstate_bv & ~state.xcr0) != 0 ||
                        (xstate_bv & ~7ULL) != 0)
                        return StepResult::GeneralProtectionFault;
                    if ((xstate_bv & 2ULL) != 0 &&
                        (get32(24) & ~state.mxcsr_mask) != 0)
                        return StepResult::GeneralProtectionFault;

                    if ((xstate_bv & 1ULL) != 0) {
                        state.x87_control_word = get16(0);
                        state.x87_status_word = get16(2);
                        state.x87_tag_word = std::to_integer<std::uint8_t>(area[4]);
                        state.x87_opcode = get16(6);
                        state.x87_instruction_pointer = get64(8);
                        state.x87_data_pointer = get64(16);
                        for (std::size_t i = 0; i < state.st.size(); ++i)
                            std::copy_n(area.begin() + 32U + i * 16U, 16,
                                        state.st[i].begin());
                    } else {
                        state.x87_control_word = 0x037f;
                        state.x87_status_word = state.x87_opcode = 0;
                        state.x87_tag_word = 0;
                        state.x87_instruction_pointer = state.x87_data_pointer = 0;
                        for (auto& reg : state.st) reg.fill(std::byte{0});
                    }
                    if ((xstate_bv & 2ULL) != 0) {
                        state.mxcsr = get32(24);
                        for (std::size_t i = 0; i < state.xmm.size(); ++i)
                            std::copy_n(area.begin() + 160U + i * 16U, 16,
                                        state.xmm[i].begin());
                    } else {
                        state.mxcsr = 0x1f80;
                        for (auto& reg : state.xmm) reg.fill(std::byte{0});
                    }
                    if ((xstate_bv & 4ULL) != 0) {
                        for (std::size_t i = 0; i < state.ymm_high.size(); ++i)
                            std::copy_n(area.begin() + 576U + i * 16U, 16,
                                        state.ymm_high[i].begin());
                    } else {
                        for (auto& reg : state.ymm_high) reg.fill(std::byte{0});
                    }
                }
                state.rip = pc;
                return StepResult::Executed;
            }
            if (operation != 0U && operation != 1U)
                return StepResult::UnsupportedInstruction;
            if ((address & 0xfU) != 0)
                return StepResult::GeneralProtectionFault;

            std::array<std::byte, 512> area{};
            const auto put16 = [&](std::size_t offset, std::uint16_t value) {
                for (std::size_t i = 0; i < 2; ++i)
                    area[offset + i] =
                        static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
            };
            const auto put32 = [&](std::size_t offset, std::uint32_t value) {
                for (std::size_t i = 0; i < 4; ++i)
                    area[offset + i] =
                        static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
            };
            const auto put64 = [&](std::size_t offset, std::uint64_t value) {
                for (std::size_t i = 0; i < 8; ++i)
                    area[offset + i] =
                        static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
            };
            const auto get16 = [&](std::size_t offset) {
                std::uint16_t value = 0;
                for (std::size_t i = 0; i < 2; ++i)
                    value |= static_cast<std::uint16_t>(
                        std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                return value;
            };
            const auto get32 = [&](std::size_t offset) {
                std::uint32_t value = 0;
                for (std::size_t i = 0; i < 4; ++i)
                    value |= static_cast<std::uint32_t>(
                        std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                return value;
            };
            const auto get64 = [&](std::size_t offset) {
                std::uint64_t value = 0;
                for (std::size_t i = 0; i < 8; ++i)
                    value |= static_cast<std::uint64_t>(
                        std::to_integer<unsigned char>(area[offset + i])) << (i * 8U);
                return value;
            };

            if (operation == 0U) { // FXSAVE
                put16(0, state.x87_control_word);
                put16(2, state.x87_status_word);
                area[4] = static_cast<std::byte>(state.x87_tag_word);
                put16(6, state.x87_opcode);
                put64(8, state.x87_instruction_pointer);
                put64(16, state.x87_data_pointer);
                put32(24, state.mxcsr);
                put32(28, state.mxcsr_mask);
                for (std::size_t i = 0; i < state.st.size(); ++i)
                    std::copy(state.st[i].begin(), state.st[i].end(),
                              area.begin() + 32U + i * 16U);
                for (std::size_t i = 0; i < state.xmm.size(); ++i)
                    std::copy(state.xmm[i].begin(), state.xmm[i].end(),
                              area.begin() + 160U + i * 16U);
                if (!memory.write(address, area)) return StepResult::MemoryFault;
            } else { // FXRSTOR
                if (!memory.read(address, area)) return StepResult::MemoryFault;
                const std::uint32_t restored_mxcsr = get32(24);
                if ((restored_mxcsr & ~state.mxcsr_mask) != 0)
                    return StepResult::GeneralProtectionFault;
                state.x87_control_word = get16(0);
                state.x87_status_word = get16(2);
                state.x87_tag_word = std::to_integer<std::uint8_t>(area[4]);
                state.x87_opcode = get16(6);
                state.x87_instruction_pointer = get64(8);
                state.x87_data_pointer = get64(16);
                state.mxcsr = restored_mxcsr;
                for (std::size_t i = 0; i < state.st.size(); ++i)
                    std::copy_n(area.begin() + 32U + i * 16U, 16, state.st[i].begin());
                for (std::size_t i = 0; i < state.xmm.size(); ++i)
                    std::copy_n(area.begin() + 160U + i * 16U, 16, state.xmm[i].begin());
            }
            state.rip = pc;
            return StepResult::Executed;
        }

        // Legacy prefixes not explicitly handled above are not silently ignored
        // by unrelated two-byte instructions (for example 66 0F 84).
        if (operand_size_override || prefix_f3 || prefix_f2)
            return StepResult::UnsupportedInstruction;

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
                eax = 0x0d;
                ebx = vendor_ebx;
                edx = vendor_edx;
                ecx = vendor_ecx;
            } else if (leaf == 1) {
                eax = 0x00000600U;
                ecx = (1U << 26U) |
                      (((state.control_registers[4] & (1ULL << 18U)) != 0)
                           ? (1U << 27U) : 0U); // XSAVE, OSXSAVE
                edx = (1U << 4U) | (1U << 5U) | (1U << 15U) | (1U << 24U); // TSC, MSR, CMOV, FXSR
            } else if (leaf == 0x0dU) {
                if (subleaf == 0) {
                    eax = 0x7U; // x87, SSE, and YMM upper halves.
                    ebx = (state.xcr0 & 4ULL) != 0 ? 832U : 576U;
                    ecx = 832U;
                }
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
            if (modrm == 0xd0) { // XGETBV
                if ((state.control_registers[4] & (1ULL << 18U)) == 0)
                    return StepResult::UnsupportedInstruction;
                if (static_cast<std::uint32_t>(state.gpr[1]) != 0)
                    return StepResult::GeneralProtectionFault;
                state.gpr[0] = static_cast<std::uint32_t>(state.xcr0);
                state.gpr[2] = static_cast<std::uint32_t>(state.xcr0 >> 32U);
                state.rip = pc;
                return StepResult::Executed;
            }
            if (modrm == 0xd1) { // XSETBV
                if (state.cpl != 0) return StepResult::PrivilegeFault;
                if ((state.control_registers[4] & (1ULL << 18U)) == 0)
                    return StepResult::UnsupportedInstruction;
                if (static_cast<std::uint32_t>(state.gpr[1]) != 0)
                    return StepResult::GeneralProtectionFault;
                const std::uint64_t value =
                    (static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(state.gpr[2])) << 32U) |
                    static_cast<std::uint32_t>(state.gpr[0]);
                // This build models x87 and SSE state only. AVX/YMM state
                // cannot be enabled until its extended state is implemented.
                if ((value & 1ULL) == 0 || (value & ~7ULL) != 0 ||
                    ((value & 2ULL) != 0 &&
                     (state.control_registers[4] & (1ULL << 9U)) == 0) ||
                    ((value & 4ULL) != 0 && (value & 2ULL) == 0))
                    return StepResult::GeneralProtectionFault;
                state.xcr0 = value;
                state.rip = pc;
                return StepResult::Executed;
            }
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

    if (operand_size_override || prefix_f3 || prefix_f2)
        return StepResult::UnsupportedInstruction;

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
        const std::size_t immediate_size = opcode == 0x83 ? 1U :
            (operand_size_override && !rex_w ? 2U : 4U);
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
        } else if (operand_size_override && !rex_w) {
            const auto raw = memory.template fetch_le<std::uint16_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            raw_immediate = *raw;
            pc += 2;
        } else {
            const auto raw = memory.template fetch_le<std::uint32_t>(pc);
            if (!raw) return StepResult::MemoryFault;
            raw_immediate = *raw;
            pc += 4;
        }

        if (operand_size_override && !rex_w) {
            const std::uint16_t rhs = opcode == 0x83
                ? static_cast<std::uint16_t>(static_cast<std::int16_t>(
                      std::bit_cast<std::int8_t>(static_cast<std::uint8_t>(raw_immediate))))
                : static_cast<std::uint16_t>(raw_immediate);
            std::uint16_t lhs = 0;
            if (mod == 3) {
                lhs = static_cast<std::uint16_t>(state.gpr[destination]);
            } else {
                const auto loaded = memory.template read_le<std::uint16_t>(address);
                if (!loaded) return StepResult::MemoryFault;
                lhs = *loaded;
            }
            std::uint16_t result = 0;
            switch (operation) {
            case 0: result = static_cast<std::uint16_t>(lhs + rhs); update_add_flags16(state, lhs, rhs, result); break;
            case 1: result = static_cast<std::uint16_t>(lhs | rhs); update_logic_flags16(state, result); break;
            case 4: result = static_cast<std::uint16_t>(lhs & rhs); update_logic_flags16(state, result); break;
            case 5: result = static_cast<std::uint16_t>(lhs - rhs); update_sub_flags16(state, lhs, rhs, result); break;
            case 6: result = static_cast<std::uint16_t>(lhs ^ rhs); update_logic_flags16(state, result); break;
            case 7: result = static_cast<std::uint16_t>(lhs - rhs); update_sub_flags16(state, lhs, rhs, result); break;
            default: return StepResult::UnsupportedInstruction;
            }
            if (operation != 7) {
                if (mod == 3)
                    state.gpr[destination] = (state.gpr[destination] & ~0xffffULL) | result;
                else if (!memory.template write_le<std::uint16_t>(address, result))
                    return StepResult::MemoryFault;
            }
        } else if (rex_w) {
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
    // Supports base, signed displacement, SIB-scaled index, no-base SIB,
    // and RIP-relative memory operands.
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
            if (operand_size_override && !rex_w) {
                const std::uint16_t lhs = static_cast<std::uint16_t>(state.gpr[destination_reg]);
                const std::uint16_t rhs = static_cast<std::uint16_t>(state.gpr[source_reg]);
                std::uint16_t result = 0;
                switch (opcode) {
                case 0x01: result = static_cast<std::uint16_t>(lhs + rhs); update_add_flags16(state, lhs, rhs, result); state.gpr[destination_reg] = (state.gpr[destination_reg] & ~0xffffULL) | result; break;
                case 0x09: result = static_cast<std::uint16_t>(lhs | rhs); update_logic_flags16(state, result); state.gpr[destination_reg] = (state.gpr[destination_reg] & ~0xffffULL) | result; break;
                case 0x21: result = static_cast<std::uint16_t>(lhs & rhs); update_logic_flags16(state, result); state.gpr[destination_reg] = (state.gpr[destination_reg] & ~0xffffULL) | result; break;
                case 0x29: result = static_cast<std::uint16_t>(lhs - rhs); update_sub_flags16(state, lhs, rhs, result); state.gpr[destination_reg] = (state.gpr[destination_reg] & ~0xffffULL) | result; break;
                case 0x31: result = static_cast<std::uint16_t>(lhs ^ rhs); update_logic_flags16(state, result); state.gpr[destination_reg] = (state.gpr[destination_reg] & ~0xffffULL) | result; break;
                case 0x39: result = static_cast<std::uint16_t>(lhs - rhs); update_sub_flags16(state, lhs, rhs, result); break;
                default: return StepResult::UnsupportedInstruction;
                }
            } else if (rex_w) {
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
        if (operand_size_override && !rex_w) {
            const auto loaded = memory.template read_le<std::uint16_t>(address);
            if (!loaded) return StepResult::MemoryFault;
            const std::uint16_t lhs = *loaded;
            const std::uint16_t rhs = static_cast<std::uint16_t>(state.gpr[source]);
            std::uint16_t result = 0;
            switch (opcode) {
            case 0x01:
                result = static_cast<std::uint16_t>(lhs + rhs);
                update_add_flags16(state, lhs, rhs, result);
                break;
            case 0x09:
                result = static_cast<std::uint16_t>(lhs | rhs);
                update_logic_flags16(state, result);
                break;
            case 0x21:
                result = static_cast<std::uint16_t>(lhs & rhs);
                update_logic_flags16(state, result);
                break;
            case 0x29:
                result = static_cast<std::uint16_t>(lhs - rhs);
                update_sub_flags16(state, lhs, rhs, result);
                break;
            case 0x31:
                result = static_cast<std::uint16_t>(lhs ^ rhs);
                update_logic_flags16(state, result);
                break;
            case 0x39:
                result = static_cast<std::uint16_t>(lhs - rhs);
                update_sub_flags16(state, lhs, rhs, result);
                break;
            default:
                return StepResult::UnsupportedInstruction;
            }
            if (opcode != 0x39 &&
                !memory.template write_le<std::uint16_t>(address, result))
                return StepResult::MemoryFault;
        } else if (rex_w) {
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
