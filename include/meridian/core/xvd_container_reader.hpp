#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace meridian::core {

struct XvdContainerInfo {
    std::uint32_t format_version{0};
    std::uint32_t volume_flags{0};
    std::uint64_t drive_size{0};
    std::uint32_t type{0};
    std::uint32_t content_type{0};
    std::uint32_t embedded_xvd_length{0};
    std::uint32_t user_data_length{0};
    std::uint32_t xvc_data_length{0};
    std::uint32_t dynamic_header_length{0};
    std::uint32_t block_size{0};
    bool is_xvc{false};
    bool is_encrypted{false};
    bool data_integrity_enabled{false};
};

// Reads public, unencrypted XVD/XVC header metadata only. It does not
// authenticate signatures, decrypt regions, or extract package contents.
class XvdContainerReader {
public:
    static constexpr std::size_t signature_size = 0x200;
    static constexpr std::size_t header_size = 0x3000;

    [[nodiscard]] static std::optional<XvdContainerInfo> inspect(
        std::span<const std::byte> file) noexcept;
};

} // namespace meridian::core
