#pragma once

#include "meridian/core/guest_memory.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <unordered_map>

namespace meridian::core {

struct PagePermissions {
    bool read{false};
    bool write{false};
    bool execute{false};
};

// A non-owning x86-64-style guest virtual address space. Mappings are
// page-granular and refer to backing GuestMemory objects owned by the caller.
class VirtualAddressSpace {
public:
    static constexpr std::uint64_t page_size = 4096;

    [[nodiscard]] bool map(std::uint64_t virtual_address,
                           GuestMemory& backing_memory,
                           std::uint64_t backing_address,
                           std::size_t length,
                           PagePermissions permissions);

    [[nodiscard]] bool unmap(std::uint64_t virtual_address,
                             std::size_t length) noexcept;

    [[nodiscard]] bool read(std::uint64_t virtual_address,
                            std::span<std::byte> out) const noexcept;
    [[nodiscard]] bool write(std::uint64_t virtual_address,
                             std::span<const std::byte> data) noexcept;
    [[nodiscard]] bool fetch(std::uint64_t virtual_address,
                             std::span<std::byte> out) const noexcept;

    [[nodiscard]] bool is_mapped(std::uint64_t virtual_address,
                                 std::size_t length = 1) const noexcept;

private:
    struct Mapping {
        GuestMemory* memory;
        std::uint64_t backing_page;
        PagePermissions permissions;
    };

    [[nodiscard]] bool validate(std::uint64_t address, std::size_t length,
                                 bool write_access, bool execute_access) const noexcept;

    std::unordered_map<std::uint64_t, Mapping> pages_;
};

} // namespace meridian::core
