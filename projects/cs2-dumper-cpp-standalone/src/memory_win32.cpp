#include "cs2dumper/memory.hpp"

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <string>
#include <vector>

namespace cs2dumper {
namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring(s.begin(), s.end());
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string narrow(const wchar_t* s) {
    if (!s || !*s) return {};
    const int len = static_cast<int>(wcslen(s));
    const int n = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

class Handle {
public:
    explicit Handle(HANDLE h = nullptr) : h_(h) {}
    ~Handle() { if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(other.h_) { other.h_ = nullptr; }
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
            h_ = other.h_; other.h_ = nullptr;
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const { return h_; }
    [[nodiscard]] bool valid() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE h_{};
};

DWORD find_process_id(const std::wstring& wanted) {
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot.valid()) throw MemoryError("CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS) failed");

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) throw MemoryError("Process32FirstW failed");

    const auto target = lower(wanted);
    do {
        if (lower(entry.szExeFile) == target) return entry.th32ProcessID;
    } while (Process32NextW(snapshot.get(), &entry));

    throw MemoryError("process not found: " + narrow(wanted.c_str()));
}

class Win32ProcessMemory final : public IProcessMemory {
public:
    explicit Win32ProcessMemory(const std::string& process_name)
        : pid_(find_process_id(widen(process_name))),
          process_(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid_)) {
        if (!process_.valid()) {
            throw MemoryError("OpenProcess failed for " + process_name + " (try running as Administrator)");
        }
    }

    bool read_raw(Address address, void* out, std::size_t size) override {
        if (size == 0) return true;
        SIZE_T read{};
        return ReadProcessMemory(process_.get(), reinterpret_cast<LPCVOID>(address), out, size, &read) != FALSE && read == size;
    }

    std::vector<ModuleInfo> module_list() override {
        Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid_));
        if (!snapshot.valid()) throw MemoryError("CreateToolhelp32Snapshot(TH32CS_SNAPMODULE) failed");

        MODULEENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        if (!Module32FirstW(snapshot.get(), &entry)) throw MemoryError("Module32FirstW failed");

        std::vector<ModuleInfo> modules;
        do {
            modules.push_back(ModuleInfo{
                narrow(entry.szModule), narrow(entry.szExePath),
                reinterpret_cast<Address>(entry.modBaseAddr),
                static_cast<std::uint64_t>(entry.modBaseSize)
            });
        } while (Module32NextW(snapshot.get(), &entry));
        return modules;
    }

    ModuleInfo module_by_name(const std::string& name) override {
        const auto wanted = lower(widen(name));
        for (auto& module : module_list()) {
            if (lower(widen(module.name)) == wanted) return module;
        }
        throw MemoryError("module not found: " + name);
    }

private:
    DWORD pid_{};
    Handle process_;
};

} // namespace

std::unique_ptr<IProcessMemory> create_native_win32_process(const MemoryBackendOptions& options) {
    return std::make_unique<Win32ProcessMemory>(options.process_name);
}

} // namespace cs2dumper

#else

namespace cs2dumper {
std::unique_ptr<IProcessMemory> create_native_win32_process(const MemoryBackendOptions&) {
    throw MemoryError("the process-memory backend is available on Windows only");
}
} // namespace cs2dumper

#endif
