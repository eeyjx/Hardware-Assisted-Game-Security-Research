#include "cs2dumper/output.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
class FakeMemory final : public cs2dumper::IProcessMemory {
public:
    bool read_raw(cs2dumper::Address address, void* out, std::size_t size) override {
        if (address == 0x1123 && size == sizeof(std::uint32_t)) {
            const std::uint32_t build = 424242;
            std::memcpy(out, &build, sizeof(build));
            return true;
        }
        return false;
    }
    std::vector<cs2dumper::ModuleInfo> module_list() override { return {module_by_name("engine2.dll")}; }
    cs2dumper::ModuleInfo module_by_name(const std::string& name) override {
        if (name == "engine2.dll") return {name, name, 0x1000, 0x1000};
        throw cs2dumper::MemoryError("not found");
    }
};

std::string read_all(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

int main() {
    cs2dumper::AnalysisResult result;
    result.buttons["attack"] = 0x10;
    result.interfaces["client.dll"]["Source2Client002"] = 0x1234;
    result.offsets["engine2.dll"]["dwBuildNumber"] = 0x123;
    result.offsets["client.dll"]["dwEntityList"] = 0xABC;

    cs2dumper::ClassInfo cls;
    cls.name = "C_Test";
    cls.module_name = "client.dll";
    cls.parent_name = "C_Base";
    cls.fields.push_back({"m_value", "int32", 0x20});
    cls.metadata.push_back({cs2dumper::ClassMetadataKind::NetworkVarNames, "m_value", "int32"});
    cs2dumper::EnumInfo en;
    en.name = "ETest";
    en.alignment = 4;
    en.size = 3;
    en.members = {{"Zero", 0}, {"One", 1}, {"Invalid", -1}};
    result.schemas["client.dll"] = {{cls}, {en}};

    const auto dir = std::filesystem::temp_directory_path() / "cs2_dumper_cpp_output_test";
    std::filesystem::remove_all(dir);
    FakeMemory mem;
    cs2dumper::Output output({"hpp", "json"}, 4, dir, result);
    output.dump_all(mem);

    for (const auto* base : {"buttons", "interfaces", "offsets", "client_dll"}) {
        for (const auto* ext : {"hpp", "json"}) {
            assert(std::filesystem::exists(dir / (std::string(base) + "." + ext)));
        }
    }
    std::size_t generated_files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.is_regular_file()) ++generated_files;
    }
    assert(generated_files == 9);
    const auto info = read_all(dir / "info.json");
    assert(info.find("424242") != std::string::npos);
    const auto schema_json = read_all(dir / "client_dll.json");
    assert(schema_json.find("\\\"C_Test\\\"") != std::string::npos);
    assert(schema_json.find("\\\"m_value\\\": 32") != std::string::npos);
    const auto hpp = read_all(dir / "client_dll.hpp");
    assert(hpp.find("inline constexpr std::ptrdiff_t m_value = 0x20;") != std::string::npos);
    assert(hpp.find("Invalid = 0xFFFFFFFF,") != std::string::npos);
    assert(hpp.find("Invalid = 0xFFFFFFFFFFFFFFFF,") == std::string::npos);

    std::filesystem::remove_all(dir);
    return 0;
}
