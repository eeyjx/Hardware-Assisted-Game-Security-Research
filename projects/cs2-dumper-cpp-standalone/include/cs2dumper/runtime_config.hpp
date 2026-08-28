#pragma once

#include "cs2dumper/types.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cs2dumper {

enum class SignatureValueKind {
    Rva,
    Immediate,
};

enum class SignatureCallback {
    None,
    ViewAngles,
    LocalPlayerPawn,
};

struct SignatureCandidate {
    std::string module;
    std::string name;
    SignatureValueKind kind{SignatureValueKind::Rva};
    SignatureCallback callback{SignatureCallback::None};
    std::string pattern;
    bool external{};
};

class SignatureDatabase {
public:
    static SignatureDatabase defaults();

    // External candidates are prepended, so a user can update a broken signature
    // without rebuilding the executable. Existing built-in candidates remain as fallbacks.
    void prepend_file(const std::filesystem::path& path);

    [[nodiscard]] std::vector<const SignatureCandidate*> candidates(
        const std::string& module, const std::string& name) const;

    [[nodiscard]] std::map<std::pair<std::string, std::string>, std::vector<const SignatureCandidate*>>
    offset_groups() const;

    [[nodiscard]] const std::optional<std::filesystem::path>& loaded_file() const { return loaded_file_; }

private:
    std::vector<SignatureCandidate> candidates_;
    std::optional<std::filesystem::path> loaded_file_;
};

class LayoutConfig {
public:
    static LayoutConfig defaults();
    void load_file(const std::filesystem::path& path);

    [[nodiscard]] std::uint64_t get(const std::string& key) const;
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] const std::optional<std::filesystem::path>& loaded_file() const { return loaded_file_; }

private:
    std::map<std::string, std::uint64_t> values_;
    std::optional<std::filesystem::path> loaded_file_;
};

} // namespace cs2dumper
