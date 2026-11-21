#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace meridian::core {

struct PeSection {
    std::string name;
    std::uint32_t virtual_address{0};
    std::uint32_t virtual_size{0};
    std::uint32_t raw_offset{0};
    std::uint32_t raw_size{0};
    std::uint32_t characteristics{0};
};

struct PeImportSymbol {
    std::string name;
    std::uint16_t ordinal{0};
    std::uint16_t hint{0};
    bool by_ordinal{false};
};

struct PeImportModule {
    std::string name;
    std::vector<PeImportSymbol> symbols;
};

struct LoadedPeImage {
    std::uint64_t image_base{0};
    std::uint64_t entry_point{0};
    std::uint32_t image_size{0};
    std::uint32_t headers_size{0};
    std::vector<std::byte> image;
    std::vector<PeSection> sections;
    std::vector<PeImportModule> imports;
};

// Minimal standard PE32+ mapper. It does not unpack Xbox XEX containers,
// resolve imports, execute TLS callbacks, or emulate loader services.
class PeImageLoader {
public:
    [[nodiscard]] static std::optional<LoadedPeImage> load(
        std::span<const std::byte> file,
        std::uint64_t requested_base = 0);
};

} // namespace meridian::core
