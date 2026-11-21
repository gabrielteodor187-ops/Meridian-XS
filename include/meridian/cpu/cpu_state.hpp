#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace meridian::cpu {

// Architectural state container for an x86-64 guest CPU.
// It stores state only; instruction decoding and execution are separate work.
struct CpuState {
    // RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI, R8-R15.
    std::array<std::uint64_t, 16> gpr{};
    std::uint64_t rip{0};
    std::uint64_t rflags{0x2}; // Reserved bit 1 is set on reset.
    // CR0-CR8 are stored by architectural register number; reserved slots remain zero.
    std::array<std::uint64_t, 9> control_registers{};
    std::uint64_t efer{0};
    std::uint64_t star{0};
    std::uint64_t lstar{0};
    std::uint64_t cstar{0};
    std::uint64_t sfmask{0};
    std::uint64_t fs_base{0};
    std::uint64_t gs_base{0};
    std::uint64_t kernel_gs_base{0};
    std::uint64_t apic_base{0};
    std::uint64_t tsc_aux{0};
    std::uint64_t tsc{0};
    std::uint64_t sysenter_cs{0};
    std::uint64_t sysenter_esp{0};
    std::uint64_t sysenter_eip{0};
    std::uint16_t cs{0};
    std::uint16_t ss{0};
    std::uint16_t ds{0};
    std::uint16_t es{0};
    std::uint16_t fs{0};
    std::uint16_t gs{0};
    std::uint8_t cpl{0};
    std::uint16_t x87_control_word{0x037f};
    std::uint16_t x87_status_word{0};
    std::uint16_t x87_opcode{0};
    std::uint8_t x87_tag_word{0};
    std::uint64_t x87_instruction_pointer{0};
    std::uint64_t x87_data_pointer{0};
    std::uint32_t mxcsr{0x1f80};
    std::uint32_t mxcsr_mask{0x0000ffbf};
    std::array<std::array<std::byte, 16>, 8> st{};
    std::array<std::array<std::byte, 16>, 16> xmm{};

    void reset() noexcept;
};

} // namespace meridian::cpu
