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
    std::uint8_t cpl{0};
    std::array<std::array<std::byte, 16>, 16> xmm{};

    void reset() noexcept;
};

} // namespace meridian::cpu
