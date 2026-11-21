#include "meridian/core/x64_paged_memory.hpp"

#include <algorithm>
#include <limits>

namespace meridian::core {

std::optional<std::uint64_t> X64PagedMemory::translate_cached(
    std::uint64_t virtual_address, X64MemoryAccess access) const noexcept {
    const std::uint64_t virtual_page = virtual_address >> 12U;
    const std::size_t slot = static_cast<std::size_t>(virtual_page % tlb_size);
    auto& entry = tlb_[slot];

    if (!entry.valid || entry.virtual_page != virtual_page) {
        const auto translation = X64PageTableWalker::translate(
            physical_memory_, cr3_, virtual_address, X64MemoryAccess::Read,
            user_mode_, write_protect_, nxe_enabled_);
        if (!translation) return std::nullopt;
        entry.virtual_page = virtual_page;
        entry.physical_page = translation->physical_address -
                              (virtual_address & 0xfffU);
        entry.writable = translation->writable;
        entry.user_accessible = translation->user_accessible;
        entry.executable = translation->executable;
        entry.valid = true;
    }

    if (user_mode_ && !entry.user_accessible) return std::nullopt;
    if (access == X64MemoryAccess::Write && !entry.writable &&
        (user_mode_ || write_protect_)) return std::nullopt;
    if (access == X64MemoryAccess::Execute && !entry.executable) return std::nullopt;
    return entry.physical_page + (virtual_address & 0xfffU);
}

void X64PagedMemory::flush_tlb() noexcept {
    for (auto& entry : tlb_) entry.valid = false;
}

void X64PagedMemory::invalidate_page(std::uint64_t virtual_address) noexcept {
    const std::uint64_t virtual_page = virtual_address >> 12U;
    const std::size_t slot = static_cast<std::size_t>(virtual_page % tlb_size);
    if (tlb_[slot].valid && tlb_[slot].virtual_page == virtual_page)
        tlb_[slot].valid = false;
}

void X64PagedMemory::set_context(std::uint64_t cr3, bool user_mode,
                                 bool write_protect, bool nxe_enabled) noexcept {
    if (cr3_ == cr3 && user_mode_ == user_mode &&
        write_protect_ == write_protect && nxe_enabled_ == nxe_enabled) return;
    cr3_ = cr3;
    user_mode_ = user_mode;
    write_protect_ = write_protect;
    nxe_enabled_ = nxe_enabled;
    flush_tlb();
}

bool X64PagedMemory::validate_range(std::uint64_t address, std::size_t length,
                                    X64MemoryAccess access) const noexcept {
    if (length == 0) return true;
    if (static_cast<std::uint64_t>(length - 1U) >
        std::numeric_limits<std::uint64_t>::max() - address) return false;

    std::uint64_t cursor = address;
    std::size_t remaining = length;
    while (remaining != 0) {
        const auto physical_address = translate_cached(cursor, access);
        if (!physical_address) return false;
        const std::size_t page_offset = static_cast<std::size_t>(cursor & 0xfffU);
        const std::size_t chunk = std::min(remaining, 4096U - page_offset);
        if (!physical_memory_.contains(*physical_address, chunk)) return false;
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
        const auto physical_address = translate_cached(cursor, X64MemoryAccess::Read);
        if (!physical_address) return false;
        const std::size_t page_offset = static_cast<std::size_t>(cursor & 0xfffU);
        const std::size_t chunk = std::min(out.size() - copied, 4096U - page_offset);
        if (!physical_memory_.read(*physical_address, out.subspan(copied, chunk)))
            return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

bool X64PagedMemory::write(std::uint64_t virtual_address,
                           std::span<const std::byte> data) noexcept {
    if (!validate_range(virtual_address, data.size(), X64MemoryAccess::Write))
        return false;
    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < data.size()) {
        const auto physical_address = translate_cached(cursor, X64MemoryAccess::Write);
        if (!physical_address) return false;
        const std::size_t page_offset = static_cast<std::size_t>(cursor & 0xfffU);
        const std::size_t chunk = std::min(data.size() - copied, 4096U - page_offset);
        if (!physical_memory_.write(*physical_address, data.subspan(copied, chunk)))
            return false;
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
        const auto physical_address = translate_cached(cursor, X64MemoryAccess::Execute);
        if (!physical_address) return false;
        const std::size_t page_offset = static_cast<std::size_t>(cursor & 0xfffU);
        const std::size_t chunk = std::min(out.size() - copied, 4096U - page_offset);
        if (!physical_memory_.read(*physical_address, out.subspan(copied, chunk)))
            return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

} // namespace meridian::core
