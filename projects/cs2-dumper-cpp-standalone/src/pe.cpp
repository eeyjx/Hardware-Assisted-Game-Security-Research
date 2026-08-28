#include "cs2dumper/pe.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace cs2dumper {
namespace {

template <typename T>
bool read_at(std::span<const std::uint8_t> image, std::size_t off, T& out) {
    if (off > image.size() || sizeof(T) > image.size() - off) return false;
    std::memcpy(&out, image.data() + off, sizeof(T));
    return true;
}

std::string_view cstr_at(std::span<const std::uint8_t> image, std::size_t off) {
    if (off >= image.size()) return {};
    const char* p = reinterpret_cast<const char*>(image.data() + off);
    const std::size_t max = image.size() - off;
    const void* end = std::memchr(p, '\0', max);
    if (!end) return {};
    return {p, static_cast<std::size_t>(static_cast<const char*>(end) - p)};
}

#pragma pack(push, 1)
struct FileHeader {
    std::uint16_t machine;
    std::uint16_t number_of_sections;
    std::uint32_t time_date_stamp;
    std::uint32_t pointer_to_symbol_table;
    std::uint32_t number_of_symbols;
    std::uint16_t size_of_optional_header;
    std::uint16_t characteristics;
};
struct SectionHeader {
    char name[8];
    std::uint32_t virtual_size;
    std::uint32_t virtual_address;
    std::uint32_t size_of_raw_data;
    std::uint32_t pointer_to_raw_data;
    std::uint32_t pointer_to_relocations;
    std::uint32_t pointer_to_linenumbers;
    std::uint16_t number_of_relocations;
    std::uint16_t number_of_linenumbers;
    std::uint32_t characteristics;
};
struct ExportDirectory {
    std::uint32_t characteristics;
    std::uint32_t time_date_stamp;
    std::uint16_t major_version;
    std::uint16_t minor_version;
    std::uint32_t name;
    std::uint32_t base;
    std::uint32_t number_of_functions;
    std::uint32_t number_of_names;
    std::uint32_t address_of_functions;
    std::uint32_t address_of_names;
    std::uint32_t address_of_name_ordinals;
};
#pragma pack(pop)

constexpr std::uint32_t IMAGE_SCN_CNT_CODE = 0x00000020u;

} // namespace

PeImage::PeImage(std::span<const std::uint8_t> image) : image_(image) {
    if (image_.size() < 0x100 || image_[0] != 'M' || image_[1] != 'Z') return;

    std::uint32_t nt_off{};
    if (!read_at(image_, 0x3C, nt_off)) return;
    if (nt_off + 4 + sizeof(FileHeader) > image_.size()) return;

    std::uint32_t sig{};
    if (!read_at(image_, nt_off, sig) || sig != 0x00004550u) return;

    FileHeader fh{};
    if (!read_at(image_, nt_off + 4, fh)) return;
    const std::size_t opt = nt_off + 4 + sizeof(FileHeader);
    if (opt + fh.size_of_optional_header > image_.size()) return;

    std::uint16_t magic{};
    if (!read_at(image_, opt, magic)) return;

    std::size_t data_dir_off{};
    if (magic == 0x20B) { // PE32+
        if (!read_at(image_, opt + 24, image_base_)) return;
        data_dir_off = opt + 112;
    } else if (magic == 0x10B) { // PE32
        std::uint32_t base32{};
        if (!read_at(image_, opt + 28, base32)) return;
        image_base_ = base32;
        data_dir_off = opt + 96;
    } else {
        return;
    }

    if (!read_at(image_, data_dir_off, export_rva_)) return;
    if (!read_at(image_, data_dir_off + 4, export_size_)) return;

    const std::size_t sec_off = opt + fh.size_of_optional_header;
    Rva first_code = 0, last_code = 0;
    for (std::uint16_t i = 0; i < fh.number_of_sections; ++i) {
        SectionHeader sh{};
        if (!read_at(image_, sec_off + static_cast<std::size_t>(i) * sizeof(SectionHeader), sh)) return;
        if ((sh.characteristics & IMAGE_SCN_CNT_CODE) != 0) {
            const auto begin = sh.virtual_address;
            const auto span = std::max(sh.virtual_size, sh.size_of_raw_data);
            const auto end64 = static_cast<std::uint64_t>(begin) + span;
            const auto end = static_cast<Rva>(std::min<std::uint64_t>(end64, image_.size()));
            if (first_code == 0 || begin < first_code) first_code = begin;
            if (end > last_code) last_code = end;
        }
    }

    if (first_code == 0 || last_code <= first_code) {
        // Fallback used only for unusual modules without a section marked CNT_CODE.
        first_code = 0;
        last_code = static_cast<Rva>(std::min<std::size_t>(image_.size(), 0xFFFFFFFFu));
    }
    code_range_ = {first_code, last_code};
    valid_ = true;
}

std::optional<Rva> PeImage::export_rva(std::string_view wanted) const {
    if (!valid_ || !export_rva_ || export_rva_ >= image_.size()) return std::nullopt;
    ExportDirectory dir{};
    if (!read_at(image_, export_rva_, dir)) return std::nullopt;

    for (std::uint32_t i = 0; i < dir.number_of_names; ++i) {
        std::uint32_t name_rva{};
        if (!read_at(image_, static_cast<std::size_t>(dir.address_of_names) + i * 4, name_rva)) return std::nullopt;
        if (cstr_at(image_, name_rva) != wanted) continue;

        std::uint16_t ordinal{};
        if (!read_at(image_, static_cast<std::size_t>(dir.address_of_name_ordinals) + i * 2, ordinal)) return std::nullopt;
        if (ordinal >= dir.number_of_functions) return std::nullopt;
        std::uint32_t function_rva{};
        if (!read_at(image_, static_cast<std::size_t>(dir.address_of_functions) + ordinal * 4, function_rva)) return std::nullopt;

        // Forwarded exports point back into the export directory. CreateInterface is not expected to be forwarded.
        if (function_rva >= export_rva_ && function_rva < static_cast<std::uint64_t>(export_rva_) + export_size_) {
            return std::nullopt;
        }
        return function_rva;
    }
    return std::nullopt;
}

} // namespace cs2dumper
