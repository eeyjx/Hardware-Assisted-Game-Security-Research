#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cs2dumper {

using Address = std::uint64_t;
using Rva = std::uint32_t;
using ButtonMap = std::map<std::string, std::int64_t>;
using InterfaceMap = std::map<std::string, std::map<std::string, std::uint64_t>>;
using OffsetMap = std::map<std::string, std::map<std::string, Rva>>;

struct ModuleInfo {
    std::string name;
    std::string path;
    Address base{};
    std::uint64_t size{};
};

enum class ClassMetadataKind {
    Unknown,
    NetworkChangeCallback,
    NetworkVarNames,
};

struct ClassMetadata {
    ClassMetadataKind kind{ClassMetadataKind::Unknown};
    std::string name;
    std::string type_name;
};

struct ClassField {
    std::string name;
    std::string type_name;
    std::int32_t offset{};
};

struct ClassInfo {
    std::string name;
    std::string module_name;
    std::optional<std::string> parent_name;
    std::vector<ClassMetadata> metadata;
    std::vector<ClassField> fields;
};

struct EnumMember {
    std::string name;
    std::int64_t value{};
};

struct EnumInfo {
    std::string name;
    std::uint8_t alignment{};
    std::uint16_t size{};
    std::vector<EnumMember> members;
};

struct TypeScope {
    std::string module_name;
    std::vector<ClassInfo> classes;
    std::vector<EnumInfo> enums;
};

using SchemaMap = std::map<std::string, std::pair<std::vector<ClassInfo>, std::vector<EnumInfo>>>;

struct AnalysisResult {
    ButtonMap buttons;
    InterfaceMap interfaces;
    OffsetMap offsets;
    SchemaMap schemas;
};

} // namespace cs2dumper
