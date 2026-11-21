#pragma once

#include "meridian/core/guest_memory.hpp"
#include "meridian/core/x64_page_table_walker.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace meridian::core {

// Virtual-memory backend that translates each guest virtual access through
// the x86-64 page tables rooted at CR3 before touching physical RAM.
class X64PagedMemory {
public:
    X64PagedMemory(GuestMemory& physical_memory, std::uint64_t cr3,
                   bool user_mode = false, bool write_protect = true,
                   bool nxe_enabled = true) noexcept
        : physical_memory_(physical_memory), cr3_(cr3), user_mode_(user_mode),
          write_protect_(write_protect), nxe_enabled_(nxe_enabled) {}

    [[nodiscard]] bool read(std::uint64_t virtual_address,
                            std::span<std::byte> out) const noexcept;
    [[nodiscard]] bool write(std::uint64_t virtual_address,
                             std::span<const std::byte> data) noexcept;
    [[nodiscard]] bool fetch(std::uint64_t virtual_address,
                             std::span<std::byte> out) const noexcept;

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] std::optional<T> read_le(std::uint64_t address) const noexcept {
        std::array<std::byte, sizeof(T)> bytes{};
        if (!read(address, bytes)) return std::nullopt;
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(std::to_integer<unsigned char>(bytes[i])) << (i * 8U);
        return value;
    }

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] std::optional<T> fetch_le(std::uint64_t address) const noexcept {
        std::array<std::byte, sizeof(T)> bytes{};
        if (!fetch(address, bytes)) return std::nullopt;
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value |= static_cast<T>(std::to_integer<unsigned char>(bytes[i])) << (i * 8U);
        return value;
    }

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] bool write_le(std::uint64_t address, T value) noexcept {
        std::array<std::byte, sizeof(T)> bytes{};
        for (std::size_t i = 0; i < sizeof(T); ++i)
            bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
        return write(address, bytes);
    }

private:
    [[nodiscard]] bool validate_range(std::uint64_t address, std::size_t length,
                                      X64MemoryAccess access) const noexcept;

    GuestMemory& physical_memory_;
    std::uint64_t cr3_;
    bool user_mode_;
    bool write_protect_;
    bool nxe_enabled_;
};

} // namespace meridian::core
