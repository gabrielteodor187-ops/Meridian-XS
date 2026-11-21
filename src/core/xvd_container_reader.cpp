#include "meridian/core/xvd_container_reader.hpp"

#include <array>
#include <limits>

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
constexpr std::size_t mutable_data_page_count_offset = 0x270;
constexpr std::size_t xvc_info_fixed_size = 0x0da8;
constexpr std::size_t xvc_region_header_size = 0x80;
constexpr std::uint64_t page_size = 0x1000;
constexpr std::uint64_t hash_entries_per_page = 0xaa;
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


bool range_valid(std::uint64_t offset, std::uint64_t length,
                 std::size_t total) noexcept {
    return offset <= total && length <= static_cast<std::uint64_t>(total) - offset;
}

bool read_u16(std::span<const std::byte> bytes, std::size_t offset,
              std::uint16_t& value) noexcept {
    if (offset > bytes.size() || bytes.size() - offset < 2) return false;
    value = static_cast<std::uint16_t>(
        std::to_integer<unsigned char>(bytes[offset])) |
        static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(
                std::to_integer<unsigned char>(bytes[offset + 1])) << 8U);
    return true;
}

std::uint64_t ceil_div(std::uint64_t value, std::uint64_t divisor) noexcept {
    return value / divisor + (value % divisor != 0 ? 1U : 0U);
}

bool add_checked(std::uint64_t a, std::uint64_t b,
                 std::uint64_t& result) noexcept {
    if (b > std::numeric_limits<std::uint64_t>::max() - a) return false;
    result = a + b;
    return true;
}

std::optional<std::uint64_t> calculate_hash_tree_pages(
    std::uint64_t hashed_pages, bool resilient) noexcept {
    std::uint64_t total = ceil_div(hashed_pages, hash_entries_per_page);
    std::uint64_t divisor = hash_entries_per_page;
    while (total > 1 && divisor <=
           std::numeric_limits<std::uint64_t>::max() / hash_entries_per_page) {
        divisor *= hash_entries_per_page;
        const std::uint64_t level_pages = ceil_div(hashed_pages, divisor);
        if (!add_checked(total, level_pages, total)) return std::nullopt;
        if (level_pages <= 1) break;
    }
    if (resilient) {
        if (total > std::numeric_limits<std::uint64_t>::max() / 2U)
            return std::nullopt;
        total *= 2U;
    }
    return total;
}

bool read_utf16_string(std::span<const std::byte> bytes, std::size_t offset,
                       std::size_t byte_length, std::string& output) {
    if (offset > bytes.size() || byte_length > bytes.size() - offset ||
        (byte_length & 1U) != 0) return false;
    output.clear();
    for (std::size_t i = 0; i < byte_length; i += 2) {
        const std::uint16_t code_unit =
            static_cast<std::uint16_t>(
                std::to_integer<unsigned char>(bytes[offset + i])) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(
                    std::to_integer<unsigned char>(bytes[offset + i + 1])) << 8U);
        if (code_unit == 0) break;
        if (code_unit > 0x7fU) output.push_back('?');
        else output.push_back(static_cast<char>(code_unit));
    }
    return true;
}

bool parse_xvc_descriptor(std::span<const std::byte> file,
                          XvdContainerInfo& info) {
    const std::uint64_t drive_pages = ceil_div(info.drive_size, page_size);
    const std::uint64_t user_pages = ceil_div(info.user_data_length, page_size);
    const std::uint64_t xvc_pages = ceil_div(info.xvc_data_length, page_size);
    const std::uint64_t dynamic_pages =
        ceil_div(info.dynamic_header_length, page_size);
    std::uint64_t hashed_pages = 0;
    if (!add_checked(drive_pages, user_pages, hashed_pages) ||
        !add_checked(hashed_pages, xvc_pages, hashed_pages) ||
        !add_checked(hashed_pages, dynamic_pages, hashed_pages)) return false;

    const bool resilient = (info.volume_flags & (1U << 4U)) != 0;
    const auto hash_pages = calculate_hash_tree_pages(hashed_pages, resilient);
    if (!hash_pages) return false;

    const std::uint64_t embedded_pages =
        ceil_div(info.embedded_xvd_length, page_size);
    std::uint64_t offset = XvdContainerReader::header_size;
    if (embedded_pages > (std::numeric_limits<std::uint64_t>::max() - offset) /
                             page_size) return false;
    offset += embedded_pages * page_size;
    const std::uint64_t mutable_bytes =
        static_cast<std::uint64_t>(info.mutable_data_page_count) * page_size;
    if (!add_checked(offset, mutable_bytes, offset)) return false;
    if (info.data_integrity_enabled) {
        if (*hash_pages > std::numeric_limits<std::uint64_t>::max() / page_size ||
            !add_checked(offset, *hash_pages * page_size, offset)) return false;
    }
    if (user_pages > std::numeric_limits<std::uint64_t>::max() / page_size ||
        !add_checked(offset, user_pages * page_size, offset)) return false;
    info.xvc_info_offset = offset;

    if (info.xvc_data_length < xvc_info_fixed_size ||
        !range_valid(offset, xvc_info_fixed_size, file.size())) return false;

    std::uint32_t version = 0;
    std::uint32_t region_count = 0;
    std::uint32_t initial_region = 0;
    std::uint64_t initial_offset = 0;
    if (!read_u32(file, static_cast<std::size_t>(offset) + 0xd10, version) ||
        !read_u32(file, static_cast<std::size_t>(offset) + 0xd14, region_count) ||
        !read_u32(file, static_cast<std::size_t>(offset) + 0xd24, initial_region) ||
        !read_u64(file, static_cast<std::size_t>(offset) + 0xd28, initial_offset))
        return false;
    info.xvc_info_version = version;
    info.initial_play_region_id = initial_region;
    info.initial_play_offset = initial_offset;
    if (version == 0) return true;
    if (region_count > 4096) return false;

    const std::uint64_t region_bytes =
        static_cast<std::uint64_t>(region_count) * xvc_region_header_size;
    const std::uint64_t descriptor_bytes = xvc_info_fixed_size + region_bytes;
    if (descriptor_bytes > info.xvc_data_length ||
        !range_valid(offset, descriptor_bytes, file.size())) return false;

    info.regions.reserve(region_count);
    for (std::uint32_t i = 0; i < region_count; ++i) {
        const std::size_t region_offset = static_cast<std::size_t>(offset) +
            xvc_info_fixed_size + static_cast<std::size_t>(i) * xvc_region_header_size;
        XvcRegionInfo region{};
        if (!read_u32(file, region_offset, region.id) ||
            !read_u16(file, region_offset + 4, region.key_id) ||
            !read_u32(file, region_offset + 8, region.flags) ||
            !read_u32(file, region_offset + 12, region.first_segment_index) ||
            !read_utf16_string(file, region_offset + 0x10, 0x40, region.description) ||
            !read_u64(file, region_offset + 0x50, region.offset) ||
            !read_u64(file, region_offset + 0x58, region.length) ||
            !read_u64(file, region_offset + 0x60, region.hash)) return false;
        info.regions.push_back(std::move(region));
    }
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
    std::span<const std::byte> file) {
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
    info.mutable_data_page_count =
        std::to_integer<std::uint8_t>(file[mutable_data_page_count_offset]);

    if (info.format_version == 0 || info.drive_size == 0 ||
        info.type > 1 || info.block_size == 0) return std::nullopt;

    info.is_xvc = is_xvc_content_type(info.content_type);
    info.is_encrypted = (info.volume_flags & encryption_disabled) == 0;
    info.data_integrity_enabled = (info.volume_flags & integrity_disabled) == 0;
    if (info.is_xvc && info.xvc_data_length != 0 &&
        !parse_xvc_descriptor(file, info)) return std::nullopt;
    return info;
}

} // namespace meridian::core
