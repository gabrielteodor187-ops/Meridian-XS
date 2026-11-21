#include "meridian/core/guest_memory.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace meridian::core {

GuestMemory::GuestMemory(std::uint64_t guest_base, std::size_t size)
    : base_(guest_base), bytes_(size) {
    if (size > std::numeric_limits<std::uint64_t>::max() - guest_base) {
        throw std::invalid_argument("guest memory range overflows the address space");
    }
}

bool GuestMemory::contains(std::uint64_t address, std::size_t length) const noexcept {
    if (address < base_) return false;
    const auto offset = address - base_;
    if (offset > bytes_.size()) return false;
    return length <= bytes_.size() - static_cast<std::size_t>(offset);
}

bool GuestMemory::read(std::uint64_t address, std::span<std::byte> out) const noexcept {
    if (!contains(address, out.size())) return false;
    const auto offset = static_cast<std::size_t>(address - base_);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset),
                out.size(), out.begin());
    return true;
}

bool GuestMemory::write(std::uint64_t address, std::span<const std::byte> data) noexcept {
    if (!contains(address, data.size())) return false;
    const auto offset = static_cast<std::size_t>(address - base_);
    std::copy(data.begin(), data.end(),
              bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    return true;
}

} // namespace meridian::core
