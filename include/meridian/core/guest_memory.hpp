#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace meridian::core {

// A bounded, byte-addressable guest RAM region. This is a host-side memory
// primitive, not a claim about the Xbox virtual-memory layout.
class GuestMemory {
public:
    GuestMemory(std::uint64_t guest_base, std::size_t size);

    [[nodiscard]] std::uint64_t base() const noexcept { return base_; }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
    [[nodiscard]] bool contains(std::uint64_t address, std::size_t length = 1) const noexcept;

    [[nodiscard]] bool read(std::uint64_t address, std::span<std::byte> out) const noexcept;
    [[nodiscard]] bool write(std::uint64_t address, std::span<const std::byte> data) noexcept;

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] std::optional<T> read_le(std::uint64_t address) const noexcept {
        if (!contains(address, sizeof(T))) return std::nullopt;
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            std::byte byte{};
            if (!read(address + i, std::span<std::byte>{&byte, 1})) return std::nullopt;
            value |= static_cast<T>(std::to_integer<unsigned char>(byte)) << (i * 8);
        }
        return value;
    }

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] std::optional<T> fetch_le(std::uint64_t address) const noexcept {
        return read_le<T>(address);
    }

    template <typename T>
        requires(std::is_integral_v<T> && std::is_unsigned_v<T>)
    [[nodiscard]] bool write_le(std::uint64_t address, T value) noexcept {
        if (!contains(address, sizeof(T))) return false;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
            const auto byte = static_cast<std::byte>((value >> (i * 8)) & 0xffU);
            if (!write(address + i, std::span<const std::byte>{&byte, 1})) return false;
        }
        return true;
    }

private:
    std::uint64_t base_;
    std::vector<std::byte> bytes_;
};

} // namespace meridian::core
