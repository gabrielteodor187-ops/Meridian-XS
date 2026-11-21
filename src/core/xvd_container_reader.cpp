#include "meridian/core/xvd_container_reader.hpp"

#include <array>

namespace meridian::core {
namespace {

constexpr std::size_t magic_offset = 0x200;
constexpr std::size_t volume_flags_offset = 0x208;
constexpr std::size_t format_version_offset = 0x20c;
constexpr std::size_t drive_size_offset = 0x218;
constexpr std::size_t type_offset = 0x280;
constexpr std::size_t content_type_offset = 0x284;
constexpr std::size_t embedded_xvd_length_offset = 0x288;
constexpr std::size_t user_data_length_offset = 0x28c;
constexpr std::size_t xvc_data_length_offset = 0x290;
constexpr std::size_t dynamic_header_length_offset = 0x294;
constexpr std::size_t block_size_offset = 0x298;
constexpr std::uint32_t encryption_disabled = 1U << 1U;
constexpr std::uint32_t integrity_disabled = 1U << 2U;

bool read_u32(std::span<const std::byte> bytes, std::size_t offset,
              std::uint32_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 4) return false;
    value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(
            std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    return true;
}

bool read_u64(std::span<const std::byte> bytes, std::size_t offset,
              std::uint64_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 8) return false;
    value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(
            std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    return true;
}

bool is_xvc_content_type(std::uint32_t content_type) noexcept {
    switch (content_type) {
    case 0x01: // Title
    case 0x06: // Application
    case 0x0f: // MteApp
    case 0x10: // MteTitle
    case 0x18: // AppDlc
    case 0x19: // TitleDlc
    case 0x1a: // UniversalDlc
        return true;
    default:
        return false;
    }
}

} // namespace

std::optional<XvdContainerInfo> XvdContainerReader::inspect(
    std::span<const std::byte> file) noexcept {
    if (file.size() < header_size) return std::nullopt;
    constexpr std::array<unsigned char, 8> magic{
        'm', 's', 'f', 't', '-', 'x', 'v', 'd'
    };
    for (std::size_t i = 0; i < magic.size(); ++i) {
        if (std::to_integer<unsigned char>(file[magic_offset + i]) != magic[i])
            return std::nullopt;
    }

    XvdContainerInfo info{};
    if (!read_u32(file, volume_flags_offset, info.volume_flags) ||
        !read_u32(file, format_version_offset, info.format_version) ||
        !read_u64(file, drive_size_offset, info.drive_size) ||
        !read_u32(file, type_offset, info.type) ||
        !read_u32(file, content_type_offset, info.content_type) ||
        !read_u32(file, embedded_xvd_length_offset, info.embedded_xvd_length) ||
        !read_u32(file, user_data_length_offset, info.user_data_length) ||
        !read_u32(file, xvc_data_length_offset, info.xvc_data_length) ||
        !read_u32(file, dynamic_header_length_offset, info.dynamic_header_length) ||
        !read_u32(file, block_size_offset, info.block_size)) return std::nullopt;

    if (info.format_version == 0 || info.drive_size == 0 ||
        info.type > 1 || info.block_size == 0) return std::nullopt;

    info.is_xvc = is_xvc_content_type(info.content_type);
    info.is_encrypted = (info.volume_flags & encryption_disabled) == 0;
    info.data_integrity_enabled = (info.volume_flags & integrity_disabled) == 0;
    return info;
}

} // namespace meridian::core
