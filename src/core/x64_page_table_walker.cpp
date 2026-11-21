#include "meridian/core/x64_page_table_walker.hpp"

namespace meridian::core {
namespace {

constexpr std::uint64_t present_bit = 1ULL << 0U;
constexpr std::uint64_t writable_bit = 1ULL << 1U;
constexpr std::uint64_t user_bit = 1ULL << 2U;
constexpr std::uint64_t page_size_bit = 1ULL << 7U;
constexpr std::uint64_t no_execute_bit = 1ULL << 63U;
constexpr std::uint64_t address_mask_4k = 0x000ffffffffff000ULL;
constexpr std::uint64_t address_mask_2m = 0x000fffffffe00000ULL;
constexpr std::uint64_t address_mask_1g = 0x000fffffc0000000ULL;

bool canonical_48(std::uint64_t address) noexcept {
    const std::uint64_t upper = address >> 48U;
    const bool sign = ((address >> 47U) & 1U) != 0;
    return sign ? upper == 0xffffU : upper == 0;
}

} // namespace

std::optional<X64PageTranslation> X64PageTableWalker::translate(
    const GuestMemory& physical_memory,
    std::uint64_t cr3,
    std::uint64_t virtual_address,
    X64MemoryAccess access,
    bool user_mode,
    bool write_protect,
    bool nxe_enabled) noexcept {
    if (!canonical_48(virtual_address)) return std::nullopt;

    std::uint64_t table = cr3 & address_mask_4k;
    bool writable = true;
    bool user_accessible = true;
    bool executable = true;

    constexpr unsigned shifts[] = {39U, 30U, 21U, 12U};
    for (unsigned level = 0; level < 4; ++level) {
        const std::uint64_t index = (virtual_address >> shifts[level]) & 0x1ffU;
        const std::uint64_t entry_address = table + index * sizeof(std::uint64_t);
        const auto entry_value = physical_memory.read_le<std::uint64_t>(entry_address);
        if (!entry_value || ((*entry_value & present_bit) == 0)) return std::nullopt;
        const std::uint64_t entry = *entry_value;

        writable = writable && ((entry & writable_bit) != 0);
        user_accessible = user_accessible && ((entry & user_bit) != 0);
        if ((entry & no_execute_bit) != 0) {
            if (!nxe_enabled) return std::nullopt;
            executable = false;
        }

        const bool large_page = (entry & page_size_bit) != 0;
        if (large_page && (level == 1 || level == 2)) {
            const std::uint64_t page_size = level == 1
                ? (1ULL << 30U) : (1ULL << 21U);
            const std::uint64_t physical_base = level == 1
                ? (entry & address_mask_1g) : (entry & address_mask_2m);
            if (user_mode && !user_accessible) return std::nullopt;
            if (access == X64MemoryAccess::Write && !writable &&
                (user_mode || write_protect)) return std::nullopt;
            if (access == X64MemoryAccess::Execute && !executable) return std::nullopt;
            return X64PageTranslation{
                physical_base | (virtual_address & (page_size - 1U)), page_size};
        }

        if (level == 3) {
            if (large_page) return std::nullopt;
            const std::uint64_t physical_base = entry & address_mask_4k;
            if (user_mode && !user_accessible) return std::nullopt;
            if (access == X64MemoryAccess::Write && !writable &&
                (user_mode || write_protect)) return std::nullopt;
            if (access == X64MemoryAccess::Execute && !executable) return std::nullopt;
            return X64PageTranslation{
                physical_base | (virtual_address & 0xfffU), 1ULL << 12U};
        }

        if (large_page) return std::nullopt;
        table = entry & address_mask_4k;
    }

    return std::nullopt;
}

} // namespace meridian::core
