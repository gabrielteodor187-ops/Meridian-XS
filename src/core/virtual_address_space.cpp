#include "meridian/core/virtual_address_space.hpp"

#include <limits>

namespace meridian::core {
namespace {

bool aligned(std::uint64_t value) noexcept {
    return value % VirtualAddressSpace::page_size == 0;
}

} // namespace

bool VirtualAddressSpace::map(std::uint64_t virtual_address,
                              GuestMemory& backing_memory,
                              std::uint64_t backing_address,
                              std::size_t length,
                              PagePermissions permissions) {
    if (length == 0 || !aligned(virtual_address) || !aligned(backing_address) ||
        length % page_size != 0 || !backing_memory.contains(backing_address, length) ||
        length > std::numeric_limits<std::uint64_t>::max() - virtual_address ||
        length > std::numeric_limits<std::uint64_t>::max() - backing_address) {
        return false;
    }

    const auto page_count = length / static_cast<std::size_t>(page_size);
    for (std::size_t i = 0; i < page_count; ++i) {
        const auto page = virtual_address + i * page_size;
        if (pages_.contains(page)) return false;
    }

    for (std::size_t i = 0; i < page_count; ++i) {
        const auto virtual_page = virtual_address + i * page_size;
        const auto backing_page = backing_address + i * page_size;
        pages_.emplace(virtual_page, Mapping{&backing_memory, backing_page, permissions});
    }
    return true;
}

bool VirtualAddressSpace::protect(std::uint64_t virtual_address,
                                  std::size_t length,
                                  PagePermissions permissions) noexcept {
    if (length == 0 || !aligned(virtual_address) || length % page_size != 0 ||
        length > std::numeric_limits<std::uint64_t>::max() - virtual_address) {
        return false;
    }

    const auto page_count = length / static_cast<std::size_t>(page_size);
    for (std::size_t i = 0; i < page_count; ++i) {
        if (!pages_.contains(virtual_address + i * page_size)) return false;
    }
    for (std::size_t i = 0; i < page_count; ++i) {
        pages_.at(virtual_address + i * page_size).permissions = permissions;
    }
    return true;
}

bool VirtualAddressSpace::unmap(std::uint64_t virtual_address,
                                std::size_t length) noexcept {
    if (length == 0 || !aligned(virtual_address) || length % page_size != 0 ||
        length > std::numeric_limits<std::uint64_t>::max() - virtual_address) {
        return false;
    }

    const auto page_count = length / static_cast<std::size_t>(page_size);
    for (std::size_t i = 0; i < page_count; ++i) {
        if (!pages_.contains(virtual_address + i * page_size)) return false;
    }
    for (std::size_t i = 0; i < page_count; ++i) {
        pages_.erase(virtual_address + i * page_size);
    }
    return true;
}

bool VirtualAddressSpace::validate(std::uint64_t address, std::size_t length,
                                   bool write_access, bool execute_access) const noexcept {
    if (length == 0) return true;
    if (length > std::numeric_limits<std::uint64_t>::max() - address) return false;

    std::uint64_t cursor = address;
    std::size_t remaining = length;
    while (remaining != 0) {
        const auto page = cursor - (cursor % page_size);
        const auto it = pages_.find(page);
        if (it == pages_.end()) return false;
        const auto& permissions = it->second.permissions;
        if (execute_access ? !permissions.execute
                           : (write_access ? !permissions.write : !permissions.read)) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(cursor - page);
        const auto chunk = static_cast<std::size_t>(page_size) - offset;
        const auto consumed = remaining < chunk ? remaining : chunk;
        cursor += consumed;
        remaining -= consumed;
    }
    return true;
}

bool VirtualAddressSpace::is_mapped(std::uint64_t virtual_address,
                                    std::size_t length) const noexcept {
    return validate(virtual_address, length, false, false);
}

bool VirtualAddressSpace::read(std::uint64_t virtual_address,
                               std::span<std::byte> out) const noexcept {
    if (!validate(virtual_address, out.size(), false, false)) return false;
    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < out.size()) {
        const auto page = cursor - (cursor % page_size);
        const auto& mapping = pages_.at(page);
        const auto offset = static_cast<std::size_t>(cursor - page);
        const auto chunk = (out.size() - copied < page_size - offset)
            ? out.size() - copied : static_cast<std::size_t>(page_size) - offset;
        if (!mapping.memory->read(mapping.backing_page + offset,
                                  out.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

bool VirtualAddressSpace::write(std::uint64_t virtual_address,
                                std::span<const std::byte> data) noexcept {
    if (!validate(virtual_address, data.size(), true, false)) return false;
    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < data.size()) {
        const auto page = cursor - (cursor % page_size);
        const auto& mapping = pages_.at(page);
        const auto offset = static_cast<std::size_t>(cursor - page);
        const auto chunk = (data.size() - copied < page_size - offset)
            ? data.size() - copied : static_cast<std::size_t>(page_size) - offset;
        if (!mapping.memory->write(mapping.backing_page + offset,
                                   data.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

bool VirtualAddressSpace::fetch(std::uint64_t virtual_address,
                                std::span<std::byte> out) const noexcept {
    if (!validate(virtual_address, out.size(), false, true)) return false;
    std::uint64_t cursor = virtual_address;
    std::size_t copied = 0;
    while (copied < out.size()) {
        const auto page = cursor - (cursor % page_size);
        const auto& mapping = pages_.at(page);
        const auto offset = static_cast<std::size_t>(cursor - page);
        const auto chunk = (out.size() - copied < page_size - offset)
            ? out.size() - copied : static_cast<std::size_t>(page_size) - offset;
        if (!mapping.memory->read(mapping.backing_page + offset,
                                  out.subspan(copied, chunk))) return false;
        cursor += chunk;
        copied += chunk;
    }
    return true;
}

} // namespace meridian::core
