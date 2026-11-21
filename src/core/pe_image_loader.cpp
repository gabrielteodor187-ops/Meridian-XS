#include "meridian/core/pe_image_loader.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace meridian::core {
namespace {

constexpr std::uint16_t pe32_plus_magic = 0x20b;
constexpr std::uint16_t amd64_machine = 0x8664;
constexpr std::uint32_t import_directory_index = 1;
constexpr std::uint32_t relocation_directory_index = 5;
constexpr std::uint32_t relocation_type_absolute = 0;
constexpr std::uint32_t relocation_type_dir64 = 10;
constexpr std::uint32_t maximum_image_size = 256U * 1024U * 1024U;

bool range_valid(std::size_t offset, std::size_t length,
                 std::size_t total) noexcept {
    return offset <= total && length <= total - offset;
}

bool read_u16(std::span<const std::byte> bytes, std::size_t offset,
              std::uint16_t& value) noexcept {
    if (!range_valid(offset, 2, bytes.size())) return false;
    value = static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset])) |
            static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(std::to_integer<std::uint8_t>(bytes[offset + 1])) << 8U);
    return true;
}

bool read_u32(std::span<const std::byte> bytes, std::size_t offset,
              std::uint32_t& value) noexcept {
    if (!range_valid(offset, 4, bytes.size())) return false;
    value = 0;
    for (std::size_t i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8U);
    return true;
}

bool read_u64(std::span<const std::byte> bytes, std::size_t offset,
              std::uint64_t& value) noexcept {
    if (!range_valid(offset, 8, bytes.size())) return false;
    value = 0;
    for (std::size_t i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(
            std::to_integer<std::uint8_t>(bytes[offset + i])) << (i * 8U);
    return true;
}

void write_u64(std::span<std::byte> bytes, std::size_t offset,
               std::uint64_t value) noexcept {
    for (std::size_t i = 0; i < 8; ++i)
        bytes[offset + i] = static_cast<std::byte>((value >> (i * 8U)) & 0xffU);
}

bool is_power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}


bool read_image_string(std::span<const std::byte> image, std::uint32_t rva,
                       std::string& value) {
    if (rva >= image.size()) return false;
    constexpr std::size_t maximum_string_length = 4096;
    value.clear();
    for (std::size_t i = 0; i < maximum_string_length; ++i) {
        const std::size_t offset = static_cast<std::size_t>(rva) + i;
        if (offset >= image.size()) return false;
        const unsigned char character =
            std::to_integer<unsigned char>(image[offset]);
        if (character == 0) return true;
        value.push_back(static_cast<char>(character));
    }
    return false;
}

bool parse_imports(LoadedPeImage& loaded, std::uint32_t import_rva,
                   std::uint32_t import_size) {
    if (import_rva == 0 && import_size == 0) return true;
    if (import_rva == 0 || import_size < 20 ||
        !range_valid(import_rva, import_size, loaded.image.size())) return false;

    const std::span<const std::byte> image{loaded.image};
    const std::size_t descriptor_count = import_size / 20U;
    bool terminated = false;
    for (std::size_t descriptor_index = 0;
         descriptor_index < descriptor_count; ++descriptor_index) {
        const std::size_t descriptor = import_rva + descriptor_index * 20U;
        std::uint32_t original_first_thunk = 0;
        std::uint32_t name_rva = 0;
        std::uint32_t first_thunk = 0;
        std::uint32_t timestamp = 0;
        std::uint32_t forwarder_chain = 0;
        if (!read_u32(image, descriptor, original_first_thunk) ||
            !read_u32(image, descriptor + 4, timestamp) ||
            !read_u32(image, descriptor + 8, forwarder_chain) ||
            !read_u32(image, descriptor + 12, name_rva) ||
            !read_u32(image, descriptor + 16, first_thunk)) return false;

        if (original_first_thunk == 0 && timestamp == 0 &&
            forwarder_chain == 0 && name_rva == 0 && first_thunk == 0) {
            terminated = true;
            break;
        }
        if (name_rva == 0 || first_thunk == 0) return false;

        PeImportModule module{};
        if (!read_image_string(image, name_rva, module.name) ||
            module.name.empty()) return false;

        const std::uint32_t lookup_rva =
            original_first_thunk != 0 ? original_first_thunk : first_thunk;
        bool thunk_terminated = false;
        const std::size_t maximum_thunks = image.size() / sizeof(std::uint64_t);
        for (std::size_t thunk_index = 0; thunk_index < maximum_thunks; ++thunk_index) {
            const std::uint64_t thunk_offset =
                static_cast<std::uint64_t>(lookup_rva) +
                thunk_index * sizeof(std::uint64_t);
            if (thunk_offset > std::numeric_limits<std::size_t>::max()) return false;
            std::uint64_t thunk = 0;
            if (!read_u64(image, static_cast<std::size_t>(thunk_offset), thunk))
                return false;
            if (thunk == 0) {
                thunk_terminated = true;
                break;
            }

            PeImportSymbol symbol{};
            if ((thunk & (1ULL << 63U)) != 0) {
                symbol.by_ordinal = true;
                symbol.ordinal = static_cast<std::uint16_t>(thunk & 0xffffU);
            } else {
                if (thunk > std::numeric_limits<std::uint32_t>::max())
                    return false;
                const auto name_entry = static_cast<std::uint32_t>(thunk);
                if (!read_u16(image, name_entry, symbol.hint) ||
                    name_entry > std::numeric_limits<std::uint32_t>::max() - 2U ||
                    !read_image_string(image, name_entry + 2U, symbol.name) ||
                    symbol.name.empty()) return false;
            }
            module.symbols.push_back(std::move(symbol));
        }
        if (!thunk_terminated) return false;
        loaded.imports.push_back(std::move(module));
    }
    return terminated;
}

bool apply_dir64_relocations(LoadedPeImage& image, std::uint32_t reloc_rva,
                             std::uint32_t reloc_size, std::uint64_t delta) {
    if (reloc_size == 0 ||
        !range_valid(reloc_rva, reloc_size, image.image.size())) return false;
    std::span<std::byte> bytes{image.image};
    std::size_t consumed = 0;
    while (consumed < reloc_size) {
        if (reloc_size - consumed < 8) return false;
        std::uint32_t page_rva = 0;
        std::uint32_t block_size = 0;
        if (!read_u32(bytes, reloc_rva + consumed, page_rva) ||
            !read_u32(bytes, reloc_rva + consumed + 4, block_size)) return false;
        if (block_size < 8 || (block_size & 1U) != 0 ||
            block_size > reloc_size - consumed) return false;

        const std::size_t entry_count = (block_size - 8U) / 2U;
        for (std::size_t i = 0; i < entry_count; ++i) {
            std::uint16_t item = 0;
            if (!read_u16(bytes, reloc_rva + consumed + 8U + i * 2U, item))
                return false;
            const std::uint16_t type = static_cast<std::uint16_t>(item >> 12U);
            const std::uint16_t offset = static_cast<std::uint16_t>(item & 0x0fffU);
            if (type == relocation_type_absolute) continue;
            if (type != relocation_type_dir64) return false;

            const std::uint64_t target = static_cast<std::uint64_t>(page_rva) + offset;
            if (target > std::numeric_limits<std::size_t>::max() ||
                !range_valid(static_cast<std::size_t>(target), 8, bytes.size())) return false;
            std::uint64_t original = 0;
            if (!read_u64(bytes, static_cast<std::size_t>(target), original)) return false;
            write_u64(bytes, static_cast<std::size_t>(target), original + delta);
        }
        consumed += block_size;
    }
    return true;
}

} // namespace

std::optional<LoadedPeImage> PeImageLoader::load(
    std::span<const std::byte> file, std::uint64_t requested_base) {
    std::uint16_t dos_magic = 0;
    std::uint32_t pe_offset = 0;
    if (!read_u16(file, 0, dos_magic) || dos_magic != 0x5a4dU ||
        !read_u32(file, 0x3c, pe_offset)) return std::nullopt;
    if (!range_valid(pe_offset, 24, file.size())) return std::nullopt;

    std::uint32_t signature = 0;
    if (!read_u32(file, pe_offset, signature) || signature != 0x00004550U)
        return std::nullopt;

    std::uint16_t machine = 0;
    std::uint16_t section_count = 0;
    std::uint16_t optional_size = 0;
    if (!read_u16(file, pe_offset + 4, machine) ||
        !read_u16(file, pe_offset + 6, section_count) ||
        !read_u16(file, pe_offset + 20, optional_size)) return std::nullopt;
    if (machine != amd64_machine || section_count == 0 || section_count > 96 ||
        optional_size < 112) return std::nullopt;

    const std::size_t optional_offset = static_cast<std::size_t>(pe_offset) + 24U;
    if (!range_valid(optional_offset, optional_size, file.size())) return std::nullopt;
    std::uint16_t magic = 0;
    std::uint32_t entry_rva = 0;
    std::uint32_t section_alignment = 0;
    std::uint32_t file_alignment = 0;
    std::uint32_t image_size = 0;
    std::uint32_t headers_size = 0;
    std::uint64_t preferred_base = 0;
    std::uint32_t directory_count = 0;
    if (!read_u16(file, optional_offset, magic) || magic != pe32_plus_magic ||
        !read_u32(file, optional_offset + 16, entry_rva) ||
        !read_u64(file, optional_offset + 24, preferred_base) ||
        !read_u32(file, optional_offset + 32, section_alignment) ||
        !read_u32(file, optional_offset + 36, file_alignment) ||
        !read_u32(file, optional_offset + 56, image_size) ||
        !read_u32(file, optional_offset + 60, headers_size) ||
        !read_u32(file, optional_offset + 108, directory_count)) return std::nullopt;
    if (!is_power_of_two(section_alignment) || !is_power_of_two(file_alignment) ||
        image_size == 0 || image_size > maximum_image_size ||
        headers_size == 0 || headers_size > image_size ||
        headers_size > file.size() || entry_rva >= image_size) return std::nullopt;

    const std::size_t section_table =
        optional_offset + static_cast<std::size_t>(optional_size);
    const std::size_t section_table_size =
        static_cast<std::size_t>(section_count) * 40U;
    if (!range_valid(section_table, section_table_size, file.size()))
        return std::nullopt;

    LoadedPeImage loaded{};
    loaded.image_base = requested_base == 0 ? preferred_base : requested_base;
    loaded.entry_point = loaded.image_base + entry_rva;
    if (loaded.entry_point < loaded.image_base) return std::nullopt;
    loaded.image_size = image_size;
    loaded.headers_size = headers_size;
    loaded.image.resize(image_size);
    loaded.sections.reserve(section_count);
    std::copy_n(file.begin(), headers_size, loaded.image.begin());

    for (std::size_t i = 0; i < section_count; ++i) {
        const std::size_t offset = section_table + i * 40U;
        PeSection section{};
        for (std::size_t n = 0; n < 8 && file[offset + n] != std::byte{0}; ++n)
            section.name.push_back(static_cast<char>(
                std::to_integer<unsigned char>(file[offset + n])));
        if (!read_u32(file, offset + 8, section.virtual_size) ||
            !read_u32(file, offset + 12, section.virtual_address) ||
            !read_u32(file, offset + 16, section.raw_size) ||
            !read_u32(file, offset + 20, section.raw_offset) ||
            !read_u32(file, offset + 36, section.characteristics)) return std::nullopt;

        const std::uint32_t mapped_size =
            std::max(section.virtual_size, section.raw_size);
        if (section.virtual_address < headers_size ||
            !range_valid(section.virtual_address, mapped_size, loaded.image.size()) ||
            !range_valid(section.raw_offset, section.raw_size, file.size()))
            return std::nullopt;
        for (const auto& existing : loaded.sections) {
            const std::uint64_t a0 = section.virtual_address;
            const std::uint64_t a1 = a0 + mapped_size;
            const std::uint64_t b0 = existing.virtual_address;
            const std::uint64_t b1 = b0 +
                std::max(existing.virtual_size, existing.raw_size);
            if (mapped_size != 0 && a0 < b1 && b0 < a1) return std::nullopt;
        }
        if (section.raw_size != 0) {
            std::copy_n(file.begin() + section.raw_offset, section.raw_size,
                        loaded.image.begin() + section.virtual_address);
        }
        loaded.sections.push_back(std::move(section));
    }

    std::uint32_t import_rva = 0;
    std::uint32_t import_size = 0;
    if (directory_count > import_directory_index &&
        optional_size >= 112U + (import_directory_index + 1U) * 8U) {
        const std::size_t directory_offset =
            optional_offset + 112U + import_directory_index * 8U;
        if (!read_u32(file, directory_offset, import_rva) ||
            !read_u32(file, directory_offset + 4, import_size)) return std::nullopt;
    }

    std::uint32_t reloc_rva = 0;
    std::uint32_t reloc_size = 0;
    if (directory_count > relocation_directory_index &&
        optional_size >= 112U + (relocation_directory_index + 1U) * 8U) {
        const std::size_t directory_offset =
            optional_offset + 112U + relocation_directory_index * 8U;
        if (!read_u32(file, directory_offset, reloc_rva) ||
            !read_u32(file, directory_offset + 4, reloc_size)) return std::nullopt;
    }

    if (loaded.image_base != preferred_base) {
        if (reloc_rva == 0 || reloc_size == 0) return std::nullopt;
        const std::uint64_t delta = loaded.image_base - preferred_base;
        if (!apply_dir64_relocations(loaded, reloc_rva, reloc_size, delta))
            return std::nullopt;
    }

    if (!parse_imports(loaded, import_rva, import_size)) return std::nullopt;
    return loaded;
}

} // namespace meridian::core
