#include "cs2dumper/analysis.hpp"

#include "cs2dumper/pattern.hpp"
#include "cs2dumper/pe.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cs2dumper {
namespace {

struct ModuleView {
    ModuleInfo module;
    std::vector<std::uint8_t> bytes;
    PeImage pe;

    ModuleView(ModuleInfo m, std::vector<std::uint8_t> b)
        : module(std::move(m)), bytes(std::move(b)), pe(bytes) {
        if (!pe.valid()) throw std::runtime_error("invalid PE image: " + module.name);
    }
};

ModuleView load_module(IProcessMemory& process, const std::string& name) {
    auto module = process.module_by_name(name);
    return ModuleView(module, process.read_bytes(module.base, static_cast<std::size_t>(module.size)));
}

Address add_signed(Address a, std::int64_t b) {
    return static_cast<Address>(static_cast<std::int64_t>(a) + b);
}

bool plausible_user_pointer(Address p) {
    return p >= 0x10000ull && p < 0x0000800000000000ull;
}

std::optional<Address> decode_factory_target(IProcessMemory& mem, Address fn, int depth = 0) {
    if (!fn || depth > 4) return std::nullopt;
    std::array<std::uint8_t, 16> b{};
    if (!mem.read_raw(fn, b.data(), b.size())) return std::nullopt;

    // Common export/function thunks.
    if (b[0] == 0xE9) {
        std::int32_t d{};
        std::memcpy(&d, b.data() + 1, sizeof(d));
        return decode_factory_target(mem, add_signed(fn + 5, d), depth + 1);
    }
    if (b[0] == 0xEB) {
        const auto d = static_cast<std::int8_t>(b[1]);
        return decode_factory_target(mem, add_signed(fn + 2, d), depth + 1);
    }

    // REX + LEA/MOV reg,[RIP+disp32]. The ModRM form with r/m=101 is RIP relative.
    if ((b[0] & 0xF0) == 0x40 && (b[1] == 0x8D || b[1] == 0x8B) && (b[2] & 0xC7) == 0x05) {
        std::int32_t d{};
        std::memcpy(&d, b.data() + 3, sizeof(d));
        const Address slot_or_object = add_signed(fn + 7, d);
        if (b[1] == 0x8D) return slot_or_object;
        try {
            const Address loaded = mem.read<std::uint64_t>(slot_or_object);
            if (plausible_user_pointer(loaded)) return loaded;
        } catch (...) {}
        return slot_or_object;
    }

    // MOV RAX, imm64.
    if (b[0] == 0x48 && b[1] == 0xB8) {
        std::uint64_t imm{};
        std::memcpy(&imm, b.data() + 2, sizeof(imm));
        if (plausible_user_pointer(imm)) return imm;
    }

    return std::nullopt;
}

std::optional<Address> decode_create_interface_list(IProcessMemory& mem, Address export_fn) {
    if (!export_fn) return std::nullopt;
    std::array<std::uint8_t, 24> b{};
    if (!mem.read_raw(export_fn, b.data(), b.size())) return std::nullopt;

    Address fn = export_fn;
    if (b[0] == 0xE9) {
        std::int32_t d{};
        std::memcpy(&d, b.data() + 1, sizeof(d));
        fn = add_signed(export_fn + 5, d);
        if (!mem.read_raw(fn, b.data(), b.size())) return std::nullopt;
    }

    // Search the first instructions for a RIP-relative load of the InterfaceReg head.
    for (std::size_t i = 0; i + 7 <= b.size(); ++i) {
        if ((b[i] & 0xF0) != 0x40 || b[i + 1] != 0x8B || (b[i + 2] & 0xC7) != 0x05) continue;
        std::int32_t d{};
        std::memcpy(&d, b.data() + i + 3, sizeof(d));
        return add_signed(fn + i + 7, d);
    }

    // Compatibility fallback for the common 7-byte first instruction.
    std::int32_t d{};
    std::memcpy(&d, b.data() + 3, sizeof(d));
    const auto candidate = add_signed(fn + 7, d);
    if (plausible_user_pointer(candidate)) return candidate;
    return std::nullopt;
}

std::map<std::string, Address> read_module_interfaces(IProcessMemory& process,
                                                      const ModuleInfo& module,
                                                      const LayoutConfig& layout) {
    const auto bytes = process.read_bytes(module.base, static_cast<std::size_t>(module.size));
    PeImage pe(bytes);
    if (!pe.valid()) return {};
    const auto create_interface = pe.export_rva("CreateInterface");
    if (!create_interface) return {};

    const auto list_slot = decode_create_interface_list(process, module.base + *create_interface);
    if (!list_slot) return {};

    Address cur{};
    try { cur = process.read<std::uint64_t>(*list_slot); }
    catch (...) { return {}; }

    const auto create_off = layout.get("interface.create_fn");
    const auto name_off = layout.get("interface.name");
    const auto next_off = layout.get("interface.next");

    std::map<std::string, Address> out;
    std::set<Address> seen;
    for (std::size_t guard = 0; cur && guard < 10000; ++guard) {
        if (!seen.insert(cur).second) break;
        try {
            const Address create_fn = process.read<std::uint64_t>(cur + create_off);
            const Address name_ptr = process.read<std::uint64_t>(cur + name_off);
            const Address next = process.read<std::uint64_t>(cur + next_off);
            const auto name = process.read_utf8(name_ptr, 256);
            if (!name.empty()) {
                if (const auto instance = decode_factory_target(process, create_fn); instance && plausible_user_pointer(*instance)) {
                    out[name] = *instance;
                }
            }
            cur = next;
        } catch (...) {
            break;
        }
    }
    return out;
}

std::optional<Address> find_interface_instance(IProcessMemory& process, const std::string& module_name,
                                               const std::string& interface_name,
                                               const LayoutConfig& layout) {
    const auto module = process.module_by_name(module_name);
    const auto interfaces = read_module_interfaces(process, module, layout);
    const auto it = interfaces.find(interface_name);
    if (it == interfaces.end()) return std::nullopt;
    return it->second;
}

std::optional<Rva> resolve_signature(const ModuleView& mv, const SignatureDatabase& db,
                                     const std::string& name) {
    PatternScanner scanner(mv.pe);
    for (const auto* candidate : db.candidates(mv.module.name, name)) {
        try {
            const auto match = scanner.find_unique_code(Pattern(candidate->pattern));
            if (!match || match->saves.size() < 2) continue;
            const auto value = match->saves[1];
            if (candidate->kind == SignatureValueKind::Rva) {
                if (value >= mv.module.size) continue;
            } else {
                if (value > 0x10000000u) continue;
            }
            return value;
        } catch (const std::exception& e) {
            std::cerr << "[warn] invalid pattern for " << name << ": " << e.what() << '\n';
        }
    }
    return std::nullopt;
}

std::vector<Address> hash_elements(IProcessMemory& mem, Address hash_base, const LayoutConfig& l) {
    const Address entry = hash_base + l.get("hash.entry_mem");
    const auto blocks_raw = mem.read<std::int32_t>(entry + l.get("memory_pool.blocks_allocated"));
    const auto peak_raw = mem.read<std::int32_t>(entry + l.get("memory_pool.peak_allocated"));
    const std::size_t blocks = blocks_raw > 0 ? static_cast<std::size_t>(blocks_raw) : 0;
    const std::size_t peak = peak_raw > 0 ? static_cast<std::size_t>(peak_raw) : 0;
    constexpr std::size_t hard_cap = 1'000'000;

    std::vector<Address> result;
    result.reserve(std::min(blocks + peak, hard_cap));

    const auto bucket_base = hash_base + l.get("hash.buckets");
    const auto bucket_count = static_cast<std::size_t>(l.get("hash.bucket_count"));
    const auto bucket_stride = l.get("hash.bucket_stride");
    const auto first_uncommitted = l.get("hash.bucket.first_uncommitted");
    const auto node_next = l.get("hash_node.next");
    const auto node_data = l.get("hash_node.data");

    std::size_t allocated_count = 0;
    for (std::size_t i = 0; i < bucket_count && allocated_count < blocks && allocated_count < hard_cap; ++i) {
        Address cur{};
        try { cur = mem.read<std::uint64_t>(bucket_base + i * bucket_stride + first_uncommitted); }
        catch (...) { continue; }
        std::size_t chain_guard = 0;
        while (cur && chain_guard++ < hard_cap && allocated_count < blocks && allocated_count < hard_cap) {
            try {
                const Address data = mem.read<std::uint64_t>(cur + node_data);
                const Address next = mem.read<std::uint64_t>(cur + node_next);
                if (data) { result.push_back(data); ++allocated_count; }
                cur = next;
            } catch (...) { break; }
        }
    }

    Address blob{};
    try { blob = mem.read<std::uint64_t>(entry + l.get("memory_pool.free_blocks_head_next")); }
    catch (...) { blob = 0; }
    std::size_t free_count = 0;
    while (blob && free_count < peak && free_count < hard_cap) {
        try {
            const Address data = mem.read<std::uint64_t>(blob + l.get("hash_blob.data"));
            const Address next = mem.read<std::uint64_t>(blob + l.get("hash_blob.next"));
            if (data) { result.push_back(data); ++free_count; }
            blob = next;
        } catch (...) { break; }
    }

    std::set<Address> seen;
    result.erase(std::remove_if(result.begin(), result.end(), [&](Address p) {
        return !plausible_user_pointer(p) || !seen.insert(p).second;
    }), result.end());
    return result;
}

ClassInfo read_class_binding(IProcessMemory& mem, Address ptr, const LayoutConfig& l) {
    ClassInfo out;
    const Address name_ptr = mem.read<std::uint64_t>(ptr + l.get("class.name"));
    const Address module_ptr = mem.read<std::uint64_t>(ptr + l.get("class.module_name"));
    out.name = mem.read_utf8(name_ptr, 4096);
    out.module_name = mem.read_utf8(module_ptr, 256);
    if (!out.module_name.empty() && !out.module_name.ends_with(".dll")) out.module_name += ".dll";
    if (out.name.empty()) throw std::runtime_error("empty class name");

    const Address bases = mem.read<std::uint64_t>(ptr + l.get("class.base_classes"));
    if (bases) {
        try {
            const Address parent = mem.read<std::uint64_t>(bases + l.get("base_class_info.class"));
            if (parent) {
                const Address parent_name = mem.read<std::uint64_t>(parent + l.get("base_class.name"));
                const auto name = mem.read_utf8(parent_name, 4096);
                if (!name.empty()) out.parent_name = name;
            }
        } catch (...) {}
    }

    const auto field_count = mem.read<std::int16_t>(ptr + l.get("class.field_count"));
    const Address fields = mem.read<std::uint64_t>(ptr + l.get("class.fields"));
    if (fields && field_count > 0 && field_count < 32767) {
        out.fields.reserve(static_cast<std::size_t>(field_count));
        const auto stride = l.get("field.stride");
        for (std::int32_t i = 0; i < field_count; ++i) {
            try {
                const Address f = fields + static_cast<Address>(i) * stride;
                const Address f_name_ptr = mem.read<std::uint64_t>(f + l.get("field.name"));
                const Address type_ptr = mem.read<std::uint64_t>(f + l.get("field.type"));
                if (!type_ptr) continue;
                const auto offset = mem.read<std::int32_t>(f + l.get("field.offset"));
                const Address type_name_ptr = mem.read<std::uint64_t>(type_ptr + l.get("schema_type.name"));
                auto type_name = mem.read_utf8(type_name_ptr, 256);
                type_name.erase(std::remove(type_name.begin(), type_name.end(), ' '), type_name.end());
                out.fields.push_back({mem.read_utf8(f_name_ptr, 4096), std::move(type_name), offset});
            } catch (...) {}
        }
    }

    const auto metadata_count = mem.read<std::int16_t>(ptr + l.get("class.metadata_count"));
    const Address metadata = mem.read<std::uint64_t>(ptr + l.get("class.metadata"));
    if (metadata && metadata_count > 0 && metadata_count < 32767) {
        out.metadata.reserve(static_cast<std::size_t>(metadata_count));
        const auto stride = l.get("metadata.stride");
        for (std::int32_t i = 0; i < metadata_count; ++i) {
            try {
                const Address md = metadata + static_cast<Address>(i) * stride;
                const Address md_name_ptr = mem.read<std::uint64_t>(md + l.get("metadata.name"));
                const Address nv = mem.read<std::uint64_t>(md + l.get("metadata.network_value"));
                if (!nv) continue;
                const auto name = mem.read_utf8(md_name_ptr, 4096);
                if (name == "MNetworkChangeCallback") {
                    const Address value = mem.read<std::uint64_t>(nv + l.get("network_value.name_ptr"));
                    out.metadata.push_back({ClassMetadataKind::NetworkChangeCallback,
                                            mem.read_utf8(value, 4096), {}});
                } else if (name == "MNetworkVarNames") {
                    const Address var_name = mem.read<std::uint64_t>(nv + l.get("network_value.var_name"));
                    const Address var_type = mem.read<std::uint64_t>(nv + l.get("network_value.var_type"));
                    auto type_name = mem.read_utf8(var_type, 256);
                    type_name.erase(std::remove(type_name.begin(), type_name.end(), ' '), type_name.end());
                    out.metadata.push_back({ClassMetadataKind::NetworkVarNames,
                                            mem.read_utf8(var_name, 4096), std::move(type_name)});
                } else {
                    out.metadata.push_back({ClassMetadataKind::Unknown, name, {}});
                }
            } catch (...) {}
        }
    }
    return out;
}

EnumInfo read_enum_binding(IProcessMemory& mem, Address ptr, const LayoutConfig& l) {
    EnumInfo out;
    const Address name_ptr = mem.read<std::uint64_t>(ptr + l.get("enum.name"));
    out.name = mem.read_utf8(name_ptr, 4096);
    if (out.name.empty()) throw std::runtime_error("empty enum name");
    out.alignment = mem.read<std::uint8_t>(ptr + l.get("enum.alignment"));
    out.size = mem.read<std::uint16_t>(ptr + l.get("enum.enumerator_count"));
    const Address enumerators = mem.read<std::uint64_t>(ptr + l.get("enum.enumerators"));
    if (enumerators && out.size > 0) {
        const auto stride = l.get("enumerator.stride");
        out.members.reserve(out.size);
        for (std::uint32_t i = 0; i < out.size; ++i) {
            try {
                const Address e = enumerators + static_cast<Address>(i) * stride;
                const Address e_name = mem.read<std::uint64_t>(e + l.get("enumerator.name"));
                const auto value = mem.read<std::uint64_t>(e + l.get("enumerator.value"));
                out.members.push_back({mem.read_utf8(e_name, 4096), static_cast<std::int64_t>(value)});
            } catch (...) {}
        }
    }
    return out;
}

bool probe_schema_system(IProcessMemory& mem, Address base, const LayoutConfig& l) {
    try {
        const Address vec = base + l.get("schema_system.type_scopes");
        const auto count = mem.read<std::int32_t>(vec + l.get("utl_vector.count"));
        const auto data = mem.read<std::uint64_t>(vec + l.get("utl_vector.data"));
        const auto registrations = mem.read<std::int32_t>(base + l.get("schema_system.registration_count"));
        return count > 0 && count < 4096 && plausible_user_pointer(data) &&
               registrations > 0 && registrations < 100000000;
    } catch (...) {
        return false;
    }
}

Address resolve_schema_system(IProcessMemory& process, const SignatureDatabase& signatures,
                              const LayoutConfig& layout) {
    if (const auto iface = find_interface_instance(process, "schemasystem.dll", "SchemaSystem_001", layout)) {
        if (probe_schema_system(process, *iface, layout)) return *iface;
    }

    const auto mv = load_module(process, "schemasystem.dll");
    if (const auto rva = resolve_signature(mv, signatures, "__SchemaSystem")) {
        const Address base = mv.module.base + *rva;
        if (probe_schema_system(process, base, layout)) return base;
    }
    throw std::runtime_error("unable to resolve a valid SchemaSystem_001 instance");
}

std::vector<TypeScope> read_type_scopes(IProcessMemory& mem, Address system, const LayoutConfig& l) {
    std::vector<TypeScope> scopes;
    const Address vec = system + l.get("schema_system.type_scopes");
    const auto count = mem.read<std::int32_t>(vec + l.get("utl_vector.count"));
    const Address data = mem.read<std::uint64_t>(vec + l.get("utl_vector.data"));
    if (count <= 0 || count > 4096 || !data) return scopes;

    for (std::int32_t i = 0; i < count; ++i) {
        try {
            const Address scope = mem.read<std::uint64_t>(data + static_cast<Address>(i) * sizeof(std::uint64_t));
            if (!scope) continue;
            TypeScope out;
            out.module_name = mem.read_utf8(scope + l.get("type_scope.name"),
                                            static_cast<std::size_t>(l.get("type_scope.name_size")));
            for (const auto ptr : hash_elements(mem, scope + l.get("type_scope.class_bindings"), l)) {
                try { out.classes.push_back(read_class_binding(mem, ptr, l)); } catch (...) {}
            }
            for (const auto ptr : hash_elements(mem, scope + l.get("type_scope.enum_bindings"), l)) {
                try { out.enums.push_back(read_enum_binding(mem, ptr, l)); } catch (...) {}
            }
            if (!out.classes.empty() || !out.enums.empty()) scopes.push_back(std::move(out));
        } catch (...) {}
    }
    return scopes;
}

void validate_runtime_offsets(IProcessMemory& process, OffsetMap& out) {
    const auto engine = out.find("engine2.dll");
    if (engine == out.end()) return;
    ModuleInfo module;
    try { module = process.module_by_name("engine2.dll"); }
    catch (...) { return; }

    auto& m = engine->second;
    if (const auto it = m.find("dwBuildNumber"); it != m.end()) {
        try {
            const auto build = process.read<std::uint32_t>(module.base + it->second);
            if (build < 1000 || build > 10000000) {
                std::cerr << "[warn] rejected implausible dwBuildNumber candidate\n";
                m.erase(it);
            }
        } catch (...) {
            m.erase(it);
        }
    }

    const auto w = m.find("dwWindowWidth");
    const auto h = m.find("dwWindowHeight");
    if (w != m.end() && h != m.end()) {
        try {
            const auto width = process.read<std::uint32_t>(module.base + w->second);
            const auto height = process.read<std::uint32_t>(module.base + h->second);
            if (width < 200 || width > 20000 || height < 200 || height > 20000) {
                std::cerr << "[warn] rejected implausible window-size signature candidates\n";
                m.erase("dwWindowWidth");
                m.erase("dwWindowHeight");
            }
        } catch (...) {}
    }
}

} // namespace

ButtonMap analyze_buttons(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout) {
    const auto mv = load_module(process, "client.dll");
    const auto rva = resolve_signature(mv, signatures, "__ButtonList");
    if (!rva) throw std::runtime_error("button list signature did not match");

    const Address list_slot = mv.module.base + *rva;
    Address cur = process.read<std::uint64_t>(list_slot);
    const auto name_off = layout.get("button.name");
    const auto state_off = layout.get("button.state");
    const auto next_off = layout.get("button.next");

    ButtonMap out;
    std::set<Address> seen;
    for (std::size_t guard = 0; cur && guard < 10000; ++guard) {
        if (!seen.insert(cur).second) break;
        const Address name_ptr = process.read<std::uint64_t>(cur + name_off);
        const auto name = process.read_utf8(name_ptr, 64);
        if (!name.empty() && cur >= mv.module.base) {
            out[name] = static_cast<std::int64_t>(cur - mv.module.base + state_off);
        }
        cur = process.read<std::uint64_t>(cur + next_off);
    }
    return out;
}

InterfaceMap analyze_interfaces(IProcessMemory& process, const LayoutConfig& layout) {
    InterfaceMap out;
    for (const auto& module : process.module_list()) {
        if (module.name == "crashandler64.dll") continue;
        try {
            auto interfaces = read_module_interfaces(process, module, layout);
            std::map<std::string, std::uint64_t> rvas;
            for (const auto& [name, instance] : interfaces) {
                if (instance >= module.base && instance < module.base + module.size) {
                    rvas[name] = instance - module.base;
                }
            }
            if (!rvas.empty()) out[module.name] = std::move(rvas);
        } catch (...) {}
    }
    return out;
}

OffsetMap analyze_offsets(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout) {
    OffsetMap out;
    std::map<std::string, ModuleView> modules;

    for (const auto& [key, candidates] : signatures.offset_groups()) {
        const auto& module_name = key.first;
        const auto& name = key.second;
        try {
            if (!modules.contains(module_name)) modules.emplace(module_name, load_module(process, module_name));
            const auto& mv = modules.at(module_name);
            const auto value = resolve_signature(mv, signatures, name);
            if (!value) {
                std::cerr << "[warn] no signature candidate matched: " << name << '\n';
                continue;
            }
            out[module_name][name] = *value;

            const auto callback = candidates.empty() ? SignatureCallback::None : candidates.front()->callback;
            if (callback == SignatureCallback::ViewAngles) {
                if (const auto extra = resolve_signature(mv, signatures, "__ViewAngles")) {
                    out[module_name]["dwViewAngles"] = *value + *extra;
                }
            } else if (callback == SignatureCallback::LocalPlayerPawn) {
                if (const auto extra = resolve_signature(mv, signatures, "__LocalPlayerPawn")) {
                    out[module_name]["dwLocalPlayerPawn"] = *value + *extra;
                }
            }
            if (name == "dwSensitivity") {
                out[module_name]["dwSensitivity_sensitivity"] =
                    static_cast<Rva>(layout.get("offset.dwSensitivity_sensitivity"));
            }
        } catch (const std::exception& e) {
            std::cerr << "[warn] failed to analyze " << module_name << " / " << name << ": " << e.what() << '\n';
        }
    }

    validate_runtime_offsets(process, out);
    return out;
}

SchemaMap analyze_schemas(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout) {
    const Address schema_system = resolve_schema_system(process, signatures, layout);
    SchemaMap out;
    for (auto& scope : read_type_scopes(process, schema_system, layout)) {
        out[scope.module_name] = {std::move(scope.classes), std::move(scope.enums)};
    }
    if (out.empty()) throw std::runtime_error("SchemaSystem resolved but no class/enum scopes were readable; check layout.cfg");
    return out;
}

AnalysisResult analyze_all(IProcessMemory& process, const SignatureDatabase& signatures,
                           const LayoutConfig& layout) {
    AnalysisResult result;
    try { result.buttons = analyze_buttons(process, signatures, layout); }
    catch (const std::exception& e) { std::cerr << "[error] failed to read buttons: " << e.what() << '\n'; }

    try { result.interfaces = analyze_interfaces(process, layout); }
    catch (const std::exception& e) { std::cerr << "[error] failed to read interfaces: " << e.what() << '\n'; }

    try { result.offsets = analyze_offsets(process, signatures, layout); }
    catch (const std::exception& e) { std::cerr << "[error] failed to read offsets: " << e.what() << '\n'; }

    try { result.schemas = analyze_schemas(process, signatures, layout); }
    catch (const std::exception& e) { std::cerr << "[error] failed to read schemas: " << e.what() << '\n'; }

    return result;
}

} // namespace cs2dumper
