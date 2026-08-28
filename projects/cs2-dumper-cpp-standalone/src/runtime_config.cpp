#include "cs2dumper/runtime_config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cs2dumper {
namespace {

std::string trim(std::string s) {
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::vector<std::string> split_pipe(const std::string& line) {
    std::vector<std::string> out;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const auto end = line.find('|', begin);
        out.push_back(trim(line.substr(begin, end == std::string::npos ? std::string::npos : end - begin)));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return out;
}

std::uint64_t parse_integer(const std::string& text) {
    const auto s = trim(text);
    if (s.empty()) throw std::runtime_error("empty integer value");
    std::size_t consumed{};
    const auto value = std::stoull(s, &consumed, 0);
    if (consumed != s.size()) throw std::runtime_error("invalid integer value: " + s);
    return value;
}

SignatureValueKind parse_kind(std::string s) {
    s = trim(std::move(s));
    if (s == "rva") return SignatureValueKind::Rva;
    if (s == "imm") return SignatureValueKind::Immediate;
    throw std::runtime_error("unknown signature value kind: " + s);
}

SignatureCallback parse_callback(std::string s) {
    s = trim(std::move(s));
    if (s == "none") return SignatureCallback::None;
    if (s == "view_angles") return SignatureCallback::ViewAngles;
    if (s == "local_player_pawn") return SignatureCallback::LocalPlayerPawn;
    throw std::runtime_error("unknown signature callback: " + s);
}


} // namespace

SignatureDatabase SignatureDatabase::defaults() {
    SignatureDatabase db;
    auto add = [&](const char* module, const char* name, SignatureValueKind kind,
                   SignatureCallback callback, const char* pattern) {
        db.candidates_.push_back({module, name, kind, callback, pattern, false});
    };

    // Special resolvers. Interface discovery is preferred for SchemaSystem;
    // these are fallback signatures only.
    add("client.dll", "__ButtonList", SignatureValueKind::Rva, SignatureCallback::None,
        "488b15${'} 4885d2 74? 488b02 4885c0");
    add("client.dll", "__ViewAngles", SignatureValueKind::Immediate, SignatureCallback::None,
        "f2420f108428u4");
    add("client.dll", "__LocalPlayerPawn", SignatureValueKind::Immediate, SignatureCallback::None,
        "4c39b6u4 74? 4488be");
    add("schemasystem.dll", "__SchemaSystem", SignatureValueKind::Rva, SignatureCallback::None,
        "4c8d35${'} 0f2845");
    add("schemasystem.dll", "__SchemaSystem", SignatureValueKind::Rva, SignatureCallback::None,
        "488905${'} 4c8d0d${} 0fb645? 4c8d45? 33f6");

    add("client.dll", "dwCSGOInput", SignatureValueKind::Rva, SignatureCallback::ViewAngles,
        "488905${'} 0f57c0 0f1105");
    add("client.dll", "dwEntityList", SignatureValueKind::Rva, SignatureCallback::None,
        "48890d${'} e9${} cc");
    add("client.dll", "dwEntityList", SignatureValueKind::Rva, SignatureCallback::None,
        "488935${'} 4885f6");
    add("client.dll", "dwGameEntitySystem", SignatureValueKind::Rva, SignatureCallback::None,
        "488b1d${'} 48891d[4] 4c63b3");
    add("client.dll", "dwGameEntitySystem_highestEntityIndex", SignatureValueKind::Immediate, SignatureCallback::None,
        "ff81u4 4885d2");
    add("client.dll", "dwGameRules", SignatureValueKind::Rva, SignatureCallback::None,
        "f6c1010f85${} 4c8b05${'} 4d85");
    add("client.dll", "dwGlobalVars", SignatureValueKind::Rva, SignatureCallback::None,
        "488915${'} 488942");
    add("client.dll", "dwGlowManager", SignatureValueKind::Rva, SignatureCallback::None,
        "488b05${'} c3 cccccccccccccccc 8b41");
    add("client.dll", "dwLocalPlayerController", SignatureValueKind::Rva, SignatureCallback::None,
        "488b05${'} 4189be");
    add("client.dll", "dwPlantedC4", SignatureValueKind::Rva, SignatureCallback::None,
        "488b1d${'} 4532f6");
    add("client.dll", "dwPrediction", SignatureValueKind::Rva, SignatureCallback::LocalPlayerPawn,
        "488d05${'} c3 cccccccccccccccc 405356 4154");
    add("client.dll", "dwSensitivity", SignatureValueKind::Rva, SignatureCallback::None,
        "488d0d${[8]'} 660f6ecd");
    add("client.dll", "dwViewMatrix", SignatureValueKind::Rva, SignatureCallback::None,
        "488d0d${'} 48c1e006");
    add("client.dll", "dwViewRender", SignatureValueKind::Rva, SignatureCallback::None,
        "488905${'} 488bc8 4885c0");
    add("client.dll", "dwWeaponC4", SignatureValueKind::Rva, SignatureCallback::None,
        "488b15${'} 488b5c24? ffc0 8905${} 488bc6 488934ea 80be");

    add("engine2.dll", "dwBuildNumber", SignatureValueKind::Rva, SignatureCallback::None,
        "8905${'} 488d0d${} ff15${} 488b0d");
    add("engine2.dll", "dwBuildNumber", SignatureValueKind::Rva, SignatureCallback::None,
        "8905${'} 488d0d${} ff15${}");
    add("engine2.dll", "dwNetworkGameClient", SignatureValueKind::Rva, SignatureCallback::None,
        "48893d${'} ff87");
    add("engine2.dll", "dwNetworkGameClient", SignatureValueKind::Rva, SignatureCallback::None,
        "48893d${'} 488d15");
    add("engine2.dll", "dwNetworkGameClient_clientTickCount", SignatureValueKind::Immediate, SignatureCallback::None,
        "8b81u4 c3 cccccccccccccccccc 8b81${} c3 cccccccccccccccccc 83b9");
    add("engine2.dll", "dwNetworkGameClient_deltaTick", SignatureValueKind::Immediate, SignatureCallback::None,
        "4c8db7u4 4c897c24");
    add("engine2.dll", "dwNetworkGameClient_deltaTick", SignatureValueKind::Immediate, SignatureCallback::None,
        "8983u4 40b7");
    add("engine2.dll", "dwNetworkGameClient_isBackgroundMap", SignatureValueKind::Immediate, SignatureCallback::None,
        "0fb681u4 c3 cccccccccccccccc 0fb681${} c3 cccccccccccccccc 4883ec");
    add("engine2.dll", "dwNetworkGameClient_localPlayer", SignatureValueKind::Immediate, SignatureCallback::None,
        "428b94d3u4 5b 49ffe3 32c0 5b c3 cccccccccccccccc 4053");
    add("engine2.dll", "dwNetworkGameClient_maxClients", SignatureValueKind::Immediate, SignatureCallback::None,
        "8b81u4 c3????????? 8b81[4] c3????????? 8b81");
    add("engine2.dll", "dwNetworkGameClient_serverTickCount", SignatureValueKind::Immediate, SignatureCallback::None,
        "8b81u4 c3 cccccccccccccccccc 83b9");
    add("engine2.dll", "dwNetworkGameClient_signOnState", SignatureValueKind::Immediate, SignatureCallback::None,
        "448b81u4 488d0d");
    add("engine2.dll", "dwWindowHeight", SignatureValueKind::Rva, SignatureCallback::None,
        "8b05${'} 8903");
    add("engine2.dll", "dwWindowWidth", SignatureValueKind::Rva, SignatureCallback::None,
        "8b05${'} 8907");

    add("inputsystem.dll", "dwInputSystem", SignatureValueKind::Rva, SignatureCallback::None,
        "488905${'} 33c0");
    add("matchmaking.dll", "dwGameTypes", SignatureValueKind::Rva, SignatureCallback::None,
        "488d0d${'} ff90");
    add("soundsystem.dll", "dwSoundSystem", SignatureValueKind::Rva, SignatureCallback::None,
        "488d0d${'} e8${} 488b0d${} [3] 4c8b82");
    add("soundsystem.dll", "dwSoundSystem_engineViewData", SignatureValueKind::Immediate, SignatureCallback::None,
        "0f1147u1 0f104e? 0f118f");

    return db;
}

void SignatureDatabase::prepend_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("unable to open signature file: " + path.string());

    std::vector<SignatureCandidate> external;
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') continue;
        const auto parts = split_pipe(line);
        if (parts.size() != 5) {
            throw std::runtime_error("invalid signature line " + std::to_string(line_no) +
                                     ": expected module|name|kind|callback|pattern");
        }
        external.push_back({parts[0], parts[1], parse_kind(parts[2]), parse_callback(parts[3]), parts[4], true});
    }

    candidates_.insert(candidates_.begin(), external.begin(), external.end());
    loaded_file_ = std::filesystem::absolute(path);
}

std::vector<const SignatureCandidate*> SignatureDatabase::candidates(
    const std::string& module, const std::string& name) const {
    std::vector<const SignatureCandidate*> out;
    for (const auto& candidate : candidates_) {
        if (candidate.module == module && candidate.name == name) out.push_back(&candidate);
    }
    return out;
}

std::map<std::pair<std::string, std::string>, std::vector<const SignatureCandidate*>>
SignatureDatabase::offset_groups() const {
    std::map<std::pair<std::string, std::string>, std::vector<const SignatureCandidate*>> out;
    for (const auto& candidate : candidates_) {
        if (candidate.name.starts_with("__")) continue;
        out[{candidate.module, candidate.name}].push_back(&candidate);
    }
    return out;
}

LayoutConfig LayoutConfig::defaults() {
    LayoutConfig c;
    auto& v = c.values_;

    v["button.name"] = 0x08;
    v["button.state"] = 0x30;
    v["button.next"] = 0x88;
    v["offset.dwSensitivity_sensitivity"] = 0x58;

    v["interface.create_fn"] = 0x00;
    v["interface.name"] = 0x08;
    v["interface.next"] = 0x10;

    v["schema_system.type_scopes"] = 0x190;
    v["schema_system.registration_count"] = 0x280;
    v["utl_vector.count"] = 0x00;
    v["utl_vector.data"] = 0x08;

    v["type_scope.name"] = 0x08;
    v["type_scope.name_size"] = 256;
    v["type_scope.class_bindings"] = 0x560;
    v["type_scope.enum_bindings"] = 0x1DD0;

    v["hash.entry_mem"] = 0x00;
    v["hash.buckets"] = 0x60;
    v["hash.bucket_count"] = 256;
    v["hash.bucket_stride"] = 0x18;
    v["hash.bucket.first_uncommitted"] = 0x10;
    v["hash_node.next"] = 0x08;
    v["hash_node.data"] = 0x10;
    v["hash_blob.next"] = 0x00;
    v["hash_blob.data"] = 0x10;

    v["memory_pool.blocks_allocated"] = 0x0C;
    v["memory_pool.peak_allocated"] = 0x10;
    v["memory_pool.free_blocks_head_next"] = 0x20;

    v["class.name"] = 0x08;
    v["class.module_name"] = 0x18;
    v["class.field_count"] = 0x24;
    v["class.metadata_count"] = 0x26;
    v["class.fields"] = 0x30;
    v["class.base_classes"] = 0x40;
    v["class.metadata"] = 0x48;

    v["base_class_info.class"] = 0x18;
    v["base_class.name"] = 0x10;

    v["field.stride"] = 0x20;
    v["field.name"] = 0x00;
    v["field.type"] = 0x08;
    v["field.offset"] = 0x10;
    v["schema_type.name"] = 0x08;

    v["metadata.stride"] = 0x10;
    v["metadata.name"] = 0x00;
    v["metadata.network_value"] = 0x08;
    v["network_value.name_ptr"] = 0x00;
    v["network_value.var_name"] = 0x00;
    v["network_value.var_type"] = 0x08;

    v["enum.name"] = 0x08;
    v["enum.alignment"] = 0x19;
    v["enum.enumerator_count"] = 0x1C;
    v["enum.enumerators"] = 0x20;
    v["enumerator.stride"] = 0x20;
    v["enumerator.name"] = 0x00;
    v["enumerator.value"] = 0x08;

    return c;
}

void LayoutConfig::load_file(const std::filesystem::path& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("unable to open layout file: " + path.string());

    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            throw std::runtime_error("invalid layout line " + std::to_string(line_no) + ": expected key=value");
        }
        const auto key = trim(line.substr(0, eq));
        const auto value = trim(line.substr(eq + 1));
        if (key.empty()) throw std::runtime_error("empty layout key on line " + std::to_string(line_no));
        values_[key] = parse_integer(value);
    }
    loaded_file_ = std::filesystem::absolute(path);
}

std::uint64_t LayoutConfig::get(const std::string& key) const {
    const auto it = values_.find(key);
    if (it == values_.end()) throw std::runtime_error("missing layout key: " + key);
    return it->second;
}

bool LayoutConfig::contains(const std::string& key) const {
    return values_.contains(key);
}

} // namespace cs2dumper
