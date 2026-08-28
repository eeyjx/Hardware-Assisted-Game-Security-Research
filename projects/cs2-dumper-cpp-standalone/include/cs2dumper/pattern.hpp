#pragma once

#include "cs2dumper/pe.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cs2dumper {

struct PatternMatch {
    std::vector<Rva> saves;
};

class Pattern {
public:
    explicit Pattern(std::string text);
    [[nodiscard]] const std::string& text() const { return text_; }

    // Public so the parser implementation can build the compact operation tree.
    enum class OpKind { Byte, Skip, Save, ReadU, FollowRel32 };
    struct Op {
        OpKind kind{};
        std::uint32_t value{};
        std::vector<Op> sub;
    };

private:
    std::string text_;
    std::vector<Op> ops_;

    friend class PatternScanner;
};

class PatternScanner {
public:
    explicit PatternScanner(const PeImage& image) : image_(image) {}

    // Mirrors pelite::Scanner::finds_code: returns a match only when exactly one code match exists.
    [[nodiscard]] std::optional<PatternMatch> find_unique_code(const Pattern& pattern) const;

private:
    const PeImage& image_;
    bool execute(const std::vector<Pattern::Op>& ops, Rva& cursor, std::vector<Rva>& saves) const;
};

} // namespace cs2dumper
