#pragma once

#include "meridian/core/guest_memory.hpp"

#include <cstdint>
#include <optional>

namespace meridian::core {

enum class X64MemoryAccess {
    Read,
    Write,
    Execute
};

struct X64PageTranslation {
    std::uint64_t physical_address{0};
    std::uint64_t page_size{0};
};

// Four-level x86-64 page-table walker for 48-bit canonical virtual addresses.
// This is an architectural primitive; CPU integration and TLB behavior are separate.
class X64PageTableWalker {
public:
    [[nodiscard]] static std::optional<X64PageTranslation> translate(
        const GuestMemory& physical_memory,
        std::uint64_t cr3,
        std::uint64_t virtual_address,
        X64MemoryAccess access,
        bool user_mode = false,
        bool write_protect = true,
        bool nxe_enabled = true) noexcept;
};

} // namespace meridian::core
