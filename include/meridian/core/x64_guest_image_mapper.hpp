#pragma once

#include "meridian/core/guest_memory.hpp"
#include "meridian/core/pe_image_loader.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace meridian::core {

struct MappedGuestImage {
    std::uint64_t cr3{0};
    std::uint64_t entry_point{0};
    std::uint64_t stack_pointer{0};
    std::size_t pages_mapped{0};
};

// Maps a loaded PE image and a small initial stack into guest physical RAM,
// constructing four-level page tables for the x86-64 interpreter backend.
class X64GuestImageMapper {
public:
    explicit X64GuestImageMapper(GuestMemory& physical_memory,
                                 std::uint64_t allocation_start = 0x1000) noexcept;

    [[nodiscard]] std::optional<MappedGuestImage> map(
        const LoadedPeImage& image,
        std::size_t stack_pages = 8,
        std::uint64_t stack_top = 0x00007ffffff00000ULL,
        bool user_accessible = true);

private:
    [[nodiscard]] std::optional<std::uint64_t> allocate_page() noexcept;
    [[nodiscard]] std::optional<std::uint64_t> ensure_table(
        std::uint64_t parent_table, std::uint64_t index);
    [[nodiscard]] bool map_page(std::uint64_t cr3, std::uint64_t virtual_address,
                                std::uint64_t physical_address,
                                bool writable, bool executable,
                                bool user_accessible);

    GuestMemory& physical_memory_;
    std::uint64_t next_free_physical_;
};

} // namespace meridian::core
