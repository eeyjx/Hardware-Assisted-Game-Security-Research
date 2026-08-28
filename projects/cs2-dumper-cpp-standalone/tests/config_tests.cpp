#include "cs2dumper/runtime_config.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main() {
    using namespace cs2dumper;

    auto db = SignatureDatabase::defaults();
    assert(!db.candidates("client.dll", "dwEntityList").empty());
    assert(!db.candidates("schemasystem.dll", "__SchemaSystem").empty());

    auto layout = LayoutConfig::defaults();
    assert(layout.get("schema_system.type_scopes") == 0x190);
    assert(layout.get("type_scope.class_bindings") == 0x560);

    const auto dir = std::filesystem::temp_directory_path() / "cs2_dumper_cpp_config_test";
    std::filesystem::create_directories(dir);
    const auto sig = dir / "signatures.cfg";
    const auto lay = dir / "layout.cfg";
    {
        std::ofstream f(sig);
        f << "client.dll|dwEntityList|rva|none|48890d${'} e9${} cc\n";
    }
    {
        std::ofstream f(lay);
        f << "schema_system.type_scopes=0x198\n";
    }
    db.prepend_file(sig);
    layout.load_file(lay);
    assert(db.candidates("client.dll", "dwEntityList").front()->external);
    assert(layout.get("schema_system.type_scopes") == 0x198);

    std::filesystem::remove_all(dir);
    return 0;
}
