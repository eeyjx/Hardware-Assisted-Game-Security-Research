#pragma once

#include "cs2dumper/types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace cs2dumper {

class PeImage {
public:
    explicit PeImage(std::span<const std::uint8_t> image);

    [[nodiscard]] bool valid() const { return valid_; }
    [[nodiscard]] std::uint64_t preferred_image_base() const { return image_base_; }
    [[nodiscard]] std::pair<Rva, Rva> code_range() const { return code_range_; }
    [[nodiscard]] std::optional<Rva> export_rva(std::string_view name) const;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const { return image_; }

private:
    std::span<const std::uint8_t> image_;
    bool valid_{};
    std::uint64_t image_base_{};
    std::pair<Rva, Rva> code_range_{};
    Rva export_rva_{};
    std::uint32_t export_size_{};
};

} // namespace cs2dumper
