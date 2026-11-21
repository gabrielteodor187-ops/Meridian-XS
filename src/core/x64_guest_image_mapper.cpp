#include "meridian/core/x64_guest_image_mapper.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace meridian::core {
namespace {

constexpr std::uint64_t page_size = 4096;
constexpr std::uint64_t present = 1ULL;
constexpr std::uint64_t writable_bit = 1ULL << 1U;
constexpr std::uint64_t user_bit = 1ULL << 2U;
constexpr std::uint64_t no_execute_bit = 1ULL << 63U;
constexpr std::uint64_t physical_address_mask = 0x000ffffffffff000ULL;
constexpr std::uint32_t section_execute = 0x20000000U;
constexpr std::uint32_t section_read = 0x40000000U;
constexpr std::uint32_t section_write = 0x80000000U;

bool canonical_address(std::uint64_t address) noexcept {
    const std::uint64_t upper = address >> 48U;
    const bool sign = ((address >> 47U) & 1U) != 0;
    return sign ? upper == 0xffffU : upper == 0;
}

} // namespace

X64GuestImageMapper::X64GuestImageMapper(
    GuestMemory& physical_memory, std::uint64_t allocation_start) noexcept
    : physical_memory_(physical_memory),
      next_free_physical_((allocation_start + page_size - 1U) & ~(page_size - 1U)) {}

std::optional<std::uint64_t> X64GuestImageMapper::allocate_page() noexcept {
    if (!physical_memory_.contains(next_free_physical_, page_size))
        return std::nullopt;
    const std::uint64_t allocated = next_free_physical_;
    if (next_free_physical_ > std::numeric_limits<std::uint64_t>::max() - page_size)
        return std::nullopt;
    next_free_physical_ += page_size;
    const std::array<std::byte, page_size> zeroes{};
    if (!physical_memory_.write(allocated, zeroes)) return std::nullopt;
    return allocated;
}

std::optional<std::uint64_t> X64GuestImageMapper::ensure_table(
    std::uint64_t parent_table, std::uint64_t index) {
    const std::uint64_t entry_address = parent_table + index * sizeof(std::uint64_t);
    const auto existing = physical_memory_.read_le<std::uint64_t>(entry_address);
    if (!existing) return std::nullopt;
    if ((*existing & present) != 0)
        return *existing & physical_address_mask;

    const auto child = allocate_page();
    if (!child) return std::nullopt;
    if (!physical_memory_.write_le<std::uint64_t>(
            entry_address, *child | present | writable_bit | user_bit))
        return std::nullopt;
    return child;
}

bool X64GuestImageMapper::map_page(std::uint64_t cr3,
                                   std::uint64_t virtual_address,
                                   std::uint64_t physical_address,
                                   bool writable, bool executable,
                                   bool user_accessible) {
    if ((virtual_address & (page_size - 1U)) != 0 ||
        (physical_address & (page_size - 1U)) != 0 ||
        !canonical_address(virtual_address)) return false;

    const std::uint64_t pml4_index = (virtual_address >> 39U) & 0x1ffU;
    const std::uint64_t pdpt_index = (virtual_address >> 30U) & 0x1ffU;
    const std::uint64_t pd_index = (virtual_address >> 21U) & 0x1ffU;
    const std::uint64_t pt_index = (virtual_address >> 12U) & 0x1ffU;

    const auto pdpt = ensure_table(cr3, pml4_index);
    if (!pdpt) return false;
    const auto pd = ensure_table(*pdpt, pdpt_index);
    if (!pd) return false;
    const auto pt = ensure_table(*pd, pd_index);
    if (!pt) return false;

    const std::uint64_t pte_address = *pt + pt_index * sizeof(std::uint64_t);
    const auto existing = physical_memory_.read_le<std::uint64_t>(pte_address);
    if (!existing || ((*existing & present) != 0)) return false;

    std::uint64_t flags = present;
    if (writable) flags |= writable_bit;
    if (user_accessible) flags |= user_bit;
    if (!executable) flags |= no_execute_bit;
    return physical_memory_.write_le<std::uint64_t>(
        pte_address, (physical_address & physical_address_mask) | flags);
}

std::optional<MappedGuestImage> X64GuestImageMapper::map(
    const LoadedPeImage& image, std::size_t stack_pages,
    std::uint64_t stack_top, bool user_accessible) {
    if (image.image.empty() || image.image.size() != image.image_size ||
        (image.image_base & (page_size - 1U)) != 0 ||
        !canonical_address(image.image_base) ||
        !canonical_address(image.entry_point) ||
        stack_pages == 0 || stack_pages > 1024 ||
        (stack_top & (page_size - 1U)) != 0 ||
        !canonical_address(stack_top)) return std::nullopt;
    if (image.image_base > std::numeric_limits<std::uint64_t>::max() -
                               image.image_size) return std::nullopt;

    const auto cr3 = allocate_page();
    if (!cr3) return std::nullopt;
    MappedGuestImage mapped{};
    mapped.cr3 = *cr3;
    mapped.entry_point = image.entry_point;

    const std::size_t image_pages =
        (image.image.size() + static_cast<std::size_t>(page_size - 1U)) /
        static_cast<std::size_t>(page_size);
    for (std::size_t page_index = 0; page_index < image_pages; ++page_index) {
        const std::uint32_t page_rva =
            static_cast<std::uint32_t>(page_index * page_size);
        const std::uint32_t page_end = static_cast<std::uint32_t>(
            std::min<std::size_t>(image.image_size, (page_index + 1U) * page_size));

        bool mapped_page = page_rva < image.headers_size;
        bool writable = false;
        bool executable = false;
        for (const auto& section : image.sections) {
            const std::uint64_t section_start = section.virtual_address;
            const std::uint64_t section_end = section_start +
                std::max(section.virtual_size, section.raw_size);
            if (section_start < page_end && section_end > page_rva) {
                mapped_page = true;
                writable = writable || ((section.characteristics & section_write) != 0);
                executable = executable || ((section.characteristics & section_execute) != 0);
            }
        }
        if (!mapped_page) continue;

        const auto physical_page = allocate_page();
        if (!physical_page) return std::nullopt;
        const std::size_t image_offset = page_index * static_cast<std::size_t>(page_size);
        const std::size_t copy_size = std::min<std::size_t>(
            page_size, image.image.size() - image_offset);
        if (!physical_memory_.write(*physical_page,
                std::span<const std::byte>{image.image}.subspan(image_offset, copy_size)))
            return std::nullopt;

        const std::uint64_t virtual_page = image.image_base + page_rva;
        if (!map_page(mapped.cr3, virtual_page, *physical_page,
                      writable, executable, user_accessible)) return std::nullopt;
        ++mapped.pages_mapped;
    }

    const std::uint64_t stack_bytes = static_cast<std::uint64_t>(stack_pages) * page_size;
    if (stack_top < stack_bytes) return std::nullopt;
    const std::uint64_t stack_base = stack_top - stack_bytes;
    for (std::size_t i = 0; i < stack_pages; ++i) {
        const auto physical_page = allocate_page();
        if (!physical_page) return std::nullopt;
        const std::uint64_t virtual_page = stack_base + i * page_size;
        if (!map_page(mapped.cr3, virtual_page, *physical_page,
                      true, false, user_accessible)) return std::nullopt;
        ++mapped.pages_mapped;
    }

    mapped.stack_pointer = stack_top;
    return mapped;
}

} // namespace meridian::core
