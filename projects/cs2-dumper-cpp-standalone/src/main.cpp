#include "cs2dumper/analysis.hpp"
#include "cs2dumper/memory.hpp"
#include "cs2dumper/output.hpp"
#include "cs2dumper/runtime_config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Args {
    std::vector<std::string> file_types{"hpp", "json"};
    std::size_t indent_size{4};
    std::filesystem::path output{"output"};
    std::string process_name{"cs2.exe"};
    std::optional<std::filesystem::path> signatures;
    std::optional<std::filesystem::path> layout;
    int verbosity{};
    bool no_log_file{};
};

[[noreturn]] void usage(int code) {
    std::ostream& out = code == 0 ? std::cout : std::cerr;
    out <<
        "cs2-dumper-cpp-standalone 1.0.0\n"
        "Self-contained C++20 runtime analyzer for Source 2 process data.\n\n"
        "Usage: cs2-dumper [OPTIONS]\n\n"
        "Options:\n"
        "  -f, --file-types <TYPES>      comma-separated: hpp,json\n"
        "  -i, --indent-size <N>         indentation width [default: 4]\n"
        "  -o, --output <PATH>           output directory [default: output]\n"
        "  -p, --process-name <NAME>     target process [default: cs2.exe]\n"
        "  -s, --signatures <PATH>       signature override file\n"
        "  -l, --layout <PATH>           Source 2 layout override file\n"
        "  -v, --verbose                 increase verbosity (repeatable)\n"
        "  -n, --no-log-file             prevent creation of cs2-dumper.log\n"
        "  -V, --version                 print version\n"
        "  -h, --help                    print help\n";
    std::exit(code);
}

std::string take_value(int& i, int argc, char** argv, std::string_view option) {
    if (++i >= argc) throw std::runtime_error("missing value for " + std::string(option));
    return argv[i];
}

std::vector<std::string> split_types(const std::string& value) {
    std::vector<std::string> out;
    std::stringstream ss(value);
    std::string item;
    while (std::getline(ss, item, ',')) if (!item.empty()) out.push_back(item);
    if (out.empty()) throw std::runtime_error("--file-types cannot be empty");
    static const std::set<std::string> supported{"hpp", "json"};
    for (const auto& type : out) {
        if (!supported.contains(type)) throw std::runtime_error("unsupported file type: " + type);
    }
    return out;
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") usage(0);
        if (arg == "-V" || arg == "--version") {
            std::cout << "cs2-dumper-cpp-standalone 1.0.0\n";
            std::exit(0);
        }
        if (arg == "-f" || arg == "--file-types") args.file_types = split_types(take_value(i, argc, argv, arg));
        else if (arg == "-i" || arg == "--indent-size") {
            const auto parsed = std::stoull(take_value(i, argc, argv, arg));
            if (parsed > 32) throw std::runtime_error("indent size must be <= 32");
            args.indent_size = static_cast<std::size_t>(parsed);
        }
        else if (arg == "-o" || arg == "--output") args.output = take_value(i, argc, argv, arg);
        else if (arg == "-p" || arg == "--process-name") args.process_name = take_value(i, argc, argv, arg);
        else if (arg == "-s" || arg == "--signatures") args.signatures = take_value(i, argc, argv, arg);
        else if (arg == "-l" || arg == "--layout") args.layout = take_value(i, argc, argv, arg);
        else if (arg == "-n" || arg == "--no-log-file") args.no_log_file = true;
        else if (arg == "-v" || arg == "--verbose") ++args.verbosity;
        else if (arg.size() > 2 && arg[0] == '-' && arg[1] == 'v' &&
                 std::all_of(arg.begin() + 1, arg.end(), [](char c) { return c == 'v'; })) {
            args.verbosity += static_cast<int>(arg.size() - 1);
        }
        else throw std::runtime_error("unknown option: " + arg);
    }
    return args;
}

std::optional<std::filesystem::path> discover_config(const std::filesystem::path& explicit_path,
                                                     const std::filesystem::path& exe_dir,
                                                     const char* filename) {
    if (!explicit_path.empty()) return explicit_path;
    for (const auto& p : {
        exe_dir / filename,
        std::filesystem::current_path() / filename,
        std::filesystem::current_path() / "config" / filename,
    }) {
        if (std::filesystem::exists(p)) return p;
    }
    return std::nullopt;
}

std::size_t nested_count(const cs2dumper::InterfaceMap& map) {
    std::size_t n = 0;
    for (const auto& [_, values] : map) n += values.size();
    return n;
}
std::size_t nested_count(const cs2dumper::OffsetMap& map) {
    std::size_t n = 0;
    for (const auto& [_, values] : map) n += values.size();
    return n;
}
std::size_t schema_count(const cs2dumper::SchemaMap& map) {
    std::size_t n = 0;
    for (const auto& [_, scope] : map) n += scope.first.size() + scope.second.size();
    return n;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto args = parse_args(argc, argv);
        const auto started = std::chrono::steady_clock::now();

        std::ofstream log_file;
        if (!args.no_log_file) {
            log_file.open("cs2-dumper.log", std::ios::trunc);
            if (!log_file) throw std::runtime_error("unable to create cs2-dumper.log");
        }
        auto log_info = [&](const std::string& text) {
            if (log_file) log_file << "[INFO] " << text << '\n';
        };

        std::filesystem::path exe_dir = std::filesystem::absolute(argv[0]).parent_path();
        auto signatures = cs2dumper::SignatureDatabase::defaults();
        auto layout = cs2dumper::LayoutConfig::defaults();

        const auto sig_path = discover_config(args.signatures.value_or(std::filesystem::path{}), exe_dir, "signatures.cfg");
        if (sig_path) {
            signatures.prepend_file(*sig_path);
            std::cout << "[+] Signature overrides: " << std::filesystem::absolute(*sig_path).string() << '\n';
        } else {
            std::cout << "[+] Signature overrides: built-in only\n";
        }

        const auto layout_path = discover_config(args.layout.value_or(std::filesystem::path{}), exe_dir, "layout.cfg");
        if (layout_path) {
            layout.load_file(*layout_path);
            std::cout << "[+] Layout overrides: " << std::filesystem::absolute(*layout_path).string() << '\n';
        } else {
            std::cout << "[+] Layout overrides: built-in only\n";
        }

        cs2dumper::MemoryBackendOptions mem_options;
        mem_options.process_name = args.process_name;
        mem_options.verbosity = args.verbosity;

        std::cout << "[+] Opening process: " << args.process_name << '\n';
        log_info("Opening process: " + args.process_name);

        auto process = cs2dumper::create_process_memory(mem_options);
        auto analysis = cs2dumper::analyze_all(*process, signatures, layout);

        std::cout << "[+] Buttons: " << analysis.buttons.size() << '\n'
                  << "[+] Interfaces: " << nested_count(analysis.interfaces) << '\n'
                  << "[+] Offsets: " << nested_count(analysis.offsets) << '\n'
                  << "[+] Schema classes/enums: " << schema_count(analysis.schemas) << '\n';

        cs2dumper::Output output(args.file_types, args.indent_size, args.output, analysis);
        output.dump_all(*process);

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        const auto done = "Dumped to " + std::filesystem::absolute(args.output).string() +
                          " in " + std::to_string(elapsed.count()) + " ms";
        std::cout << "[+] " << done << '\n';
        log_info(done);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[!] " << ex.what() << '\n';
        return 1;
    }
}
