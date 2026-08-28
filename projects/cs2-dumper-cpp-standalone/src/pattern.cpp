#include "cs2dumper/pattern.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

namespace cs2dumper {
namespace {

class Parser {
public:
    explicit Parser(std::string_view s) : s_(s) {}

    std::vector<Pattern::Op> parse_until(char terminator = '\0') {
        std::vector<Pattern::Op> out;
        while (true) {
            skip_spaces();
            if (pos_ >= s_.size()) {
                if (terminator != '\0') throw std::runtime_error("unterminated pattern subexpression");
                break;
            }
            if (terminator != '\0' && s_[pos_] == terminator) {
                ++pos_;
                break;
            }

            const char c = s_[pos_];
            if (c == '?') {
                ++pos_;
                out.push_back({Pattern::OpKind::Skip, 1, {}});
            } else if (c == '\'') {
                ++pos_;
                out.push_back({Pattern::OpKind::Save, 0, {}});
            } else if (c == '[') {
                ++pos_;
                std::uint32_t n = 0;
                bool any = false;
                while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                    any = true;
                    n = n * 10u + static_cast<unsigned>(s_[pos_] - '0');
                    ++pos_;
                }
                if (!any || pos_ >= s_.size() || s_[pos_] != ']') throw std::runtime_error("invalid [N] in pattern");
                ++pos_;
                out.push_back({Pattern::OpKind::Skip, n, {}});
            } else if (c == 'u') {
                ++pos_;
                if (pos_ >= s_.size() || (s_[pos_] != '1' && s_[pos_] != '2' && s_[pos_] != '4')) {
                    throw std::runtime_error("only u1/u2/u4 are supported in this port");
                }
                const auto n = static_cast<std::uint32_t>(s_[pos_] - '0');
                ++pos_;
                out.push_back({Pattern::OpKind::ReadU, n, {}});
            } else if (c == '$') {
                ++pos_;
                skip_spaces();
                std::vector<Pattern::Op> sub;
                if (pos_ < s_.size() && s_[pos_] == '{') {
                    ++pos_;
                    sub = parse_until('}');
                }
                out.push_back({Pattern::OpKind::FollowRel32, 0, std::move(sub)});
            } else if (is_hex(c)) {
                const int hi = hex_value(c);
                ++pos_;
                skip_spaces();
                if (pos_ >= s_.size() || !is_hex(s_[pos_])) throw std::runtime_error("odd hex digit in pattern");
                const int lo = hex_value(s_[pos_++]);
                out.push_back({Pattern::OpKind::Byte, static_cast<std::uint32_t>((hi << 4) | lo), {}});
            } else {
                throw std::runtime_error(std::string("unsupported pattern token: ") + c);
            }
        }
        return out;
    }

private:
    static bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
    static int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return 10 + (c - 'a');
    }
    void skip_spaces() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) ++pos_;
    }

    std::string_view s_;
    std::size_t pos_{};
};

std::optional<std::uint8_t> first_literal(const std::vector<Pattern::Op>& ops) {
    for (const auto& op : ops) {
        if (op.kind == Pattern::OpKind::Byte) return static_cast<std::uint8_t>(op.value);
        if (op.kind == Pattern::OpKind::Skip || op.kind == Pattern::OpKind::ReadU || op.kind == Pattern::OpKind::FollowRel32) return std::nullopt;
    }
    return std::nullopt;
}

} // namespace

Pattern::Pattern(std::string text) : text_(std::move(text)) {
    Parser p(text_);
    ops_ = p.parse_until();
}

bool PatternScanner::execute(const std::vector<Pattern::Op>& ops, Rva& cursor, std::vector<Rva>& saves) const {
    const auto bytes = image_.bytes();
    for (const auto& op : ops) {
        switch (op.kind) {
        case Pattern::OpKind::Byte:
            if (cursor >= bytes.size() || bytes[cursor] != static_cast<std::uint8_t>(op.value)) return false;
            ++cursor;
            break;
        case Pattern::OpKind::Skip:
            if (static_cast<std::uint64_t>(cursor) + op.value > bytes.size()) return false;
            cursor += op.value;
            break;
        case Pattern::OpKind::Save:
            saves.push_back(cursor);
            break;
        case Pattern::OpKind::ReadU: {
            if (static_cast<std::uint64_t>(cursor) + op.value > bytes.size()) return false;
            std::uint32_t v = 0;
            std::memcpy(&v, bytes.data() + cursor, op.value);
            saves.push_back(v);
            cursor += op.value;
            break;
        }
        case Pattern::OpKind::FollowRel32: {
            if (static_cast<std::uint64_t>(cursor) + 4 > bytes.size()) return false;
            std::int32_t disp{};
            std::memcpy(&disp, bytes.data() + cursor, sizeof(disp));
            const std::int64_t target64 = static_cast<std::int64_t>(cursor) + 4 + disp;
            if (target64 < 0 || static_cast<std::uint64_t>(target64) >= bytes.size()) return false;
            Rva target = static_cast<Rva>(target64);
            if (!op.sub.empty() && !execute(op.sub, target, saves)) return false;
            cursor += 4;
            break;
        }
        }
    }
    return true;
}

std::optional<PatternMatch> PatternScanner::find_unique_code(const Pattern& pattern) const {
    const auto [begin, end] = image_.code_range();
    if (begin >= end || end > image_.bytes().size()) return std::nullopt;

    std::optional<PatternMatch> found;
    const auto literal = first_literal(pattern.ops_);
    for (Rva start = begin; start < end; ++start) {
        if (literal && image_.bytes()[start] != *literal) continue;
        Rva cursor = start;
        std::vector<Rva> saves;
        saves.reserve(4);
        saves.push_back(start); // pelite reserves save[0] for the match RVA.
        if (execute(pattern.ops_, cursor, saves)) {
            if (found.has_value()) return std::nullopt; // finds_code requires uniqueness.
            found = PatternMatch{std::move(saves)};
        }
    }
    return found;
}

} // namespace cs2dumper
