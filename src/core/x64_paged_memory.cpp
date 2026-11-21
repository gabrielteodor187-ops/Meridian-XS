#include "meridian/core/x64_paged_memory.hpp"

#include <algorithm>
#include <limits>

namespace meridian::core {

bool X64PagedMemory::validate_range(std::uint64_t address, std::size_t length,
                                    X64MemoryAccess access) const noexcept {
    if (length == 0) return true;
    if (static_cast<std::uint64_t>(length - 1U) >
        std::numeric_limits<std::uint64_t>::max() - address) return false;

    std::uint64_t cursor = address;
    std::size_t remaining = length;
    while (remaining != 0) {
        const auto translation = X64PageTableWalker::translate(
            physical_memory_, cr3_, cursor, access,
            user_mode_, write_protect_, nxe_enabled_);
        if (!translation) return false;
        const std::uint64_t page_offset =
            cursor & (translation->page_size - 1U);
        const std::uint64_t available =
            translation->page_size - page_offset;
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, available));
        if (!physical_memory_.contains(translation->physical_address, chunk))
            return false;
        cursor += chunk;
        remaining -= chunk;
    }
    return true;
}

bool X64PagedMemory::read(std::uint64_t virtual_address,
                          std::span<std::byte> out) const noexcept {
    if (!validate_range(virtual_address, out.size(), X64MemoryAccess::Read))
        return false;

    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < out.size()) {
        const auto translation = X64PageTableWalker::translate(
            physical_memory_, cr3_, cursor, X64MemoryAccess::Read,
            user_mode_, write_protect_, nxe_enabled_);
        if (!translation) return false;
        const std::uint64_t page_offset = cursor & (translation->page_size - 1U);
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(out.size() - copied,
                                    translation->page_size - page_offset));
        if (!physical_memory_.read(translation->physical_address,
                                   out.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

bool X64PagedMemory::write(std::uint64_t virtual_address,
                           std::span<const std::byte> data) noexcept {
    // Validate the entire range first so a cross-page fault cannot partially write.
    if (!validate_range(virtual_address, data.size(), X64MemoryAccess::Write))
        return false;

    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < data.size()) {
        const auto translation = X64PageTableWalker::translate(
            physical_memory_, cr3_, cursor, X64MemoryAccess::Write,
            user_mode_, write_protect_, nxe_enabled_);
        if (!translation) return false;
        const std::uint64_t page_offset = cursor & (translation->page_size - 1U);
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(data.size() - copied,
                                    translation->page_size - page_offset));
        if (!physical_memory_.write(translation->physical_address,
                                    data.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

bool X64PagedMemory::fetch(std::uint64_t virtual_address,
                           std::span<std::byte> out) const noexcept {
    if (!validate_range(virtual_address, out.size(), X64MemoryAccess::Execute))
        return false;

    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < out.size()) {
        const auto translation = X64PageTableWalker::translate(
            physical_memory_, cr3_, cursor, X64MemoryAccess::Execute,
            user_mode_, write_protect_, nxe_enabled_);
        if (!translation) return false;
        const std::uint64_t page_offset = cursor & (translation->page_size - 1U);
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(out.size() - copied,
                                    translation->page_size - page_offset));
        if (!physical_memory_.read(translation->physical_address,
                                   out.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

} // namespace meridian::core
