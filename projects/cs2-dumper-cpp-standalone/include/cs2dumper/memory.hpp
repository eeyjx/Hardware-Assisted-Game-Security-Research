#pragma once

#include "cs2dumper/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace cs2dumper {

class MemoryError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class IProcessMemory {
public:
    virtual ~IProcessMemory() = default;

    virtual bool read_raw(Address address, void* out, std::size_t size) = 0;
    virtual std::vector<ModuleInfo> module_list() = 0;
    virtual ModuleInfo module_by_name(const std::string& name) = 0;

    template <typename T>
    T read(Address address) {
        static_assert(std::is_trivially_copyable_v<T>);
        T value{};
        if (!read_raw(address, &value, sizeof(T))) {
            throw MemoryError("failed to read process memory at 0x" + hex(address));
        }
        return value;
    }

    std::vector<std::uint8_t> read_bytes(Address address, std::size_t size) {
        std::vector<std::uint8_t> out(size);
        if (size && !read_raw(address, out.data(), out.size())) {
            throw MemoryError("failed to read process memory at 0x" + hex(address));
        }
        return out;
    }

    std::string read_utf8(Address address, std::size_t max_len, bool lossy = true) {
        if (!address || max_len == 0) return {};
        std::vector<char> buf(max_len, '\0');
        if (!read_raw(address, buf.data(), buf.size())) {
            if (lossy) {
                std::string result;
                result.reserve(std::min<std::size_t>(max_len, 256));
                for (std::size_t i = 0; i < max_len; ++i) {
                    char c{};
                    if (!read_raw(address + i, &c, 1) || c == '\0') break;
                    result.push_back(c);
                }
                return result;
            }
            throw MemoryError("failed to read string at 0x" + hex(address));
        }
        const auto end = std::find(buf.begin(), buf.end(), '\0');
        return std::string(buf.begin(), end);
    }

    static std::string hex(std::uint64_t value);
};

struct MemoryBackendOptions {
    std::string process_name{"cs2.exe"};
    int verbosity{};
};

std::unique_ptr<IProcessMemory> create_process_memory(const MemoryBackendOptions& options);

} // namespace cs2dumper
