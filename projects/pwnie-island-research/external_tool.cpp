#include "shared_state.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}

    ~UniqueHandle() {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }

    void reset(HANDLE replacement = nullptr) noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            CloseHandle(value_);
        }
        value_ = replacement;
    }

private:
    HANDLE value_ = nullptr;
};

class UniqueView {
public:
    UniqueView() noexcept = default;
    explicit UniqueView(void* value) noexcept : value_(value) {}

    ~UniqueView() {
        reset();
    }

    UniqueView(const UniqueView&) = delete;
    UniqueView& operator=(const UniqueView&) = delete;

    UniqueView(UniqueView&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}

    UniqueView& operator=(UniqueView&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] void* get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

    void reset(void* replacement = nullptr) noexcept {
        if (value_ != nullptr) {
            UnmapViewOfFile(value_);
        }
        value_ = replacement;
    }

private:
    void* value_ = nullptr;
};

[[nodiscard]] std::optional<DWORD>
FindProcessId(std::wstring_view executable_name) {
    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        std::wcerr << L"Process snapshot failed. Error: "
                   << GetLastError() << L'\n';
        return std::nullopt;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Process32FirstW(snapshot.get(), &entry)) {
        std::wcerr << L"Process32FirstW failed. Error: "
                   << GetLastError() << L'\n';
        return std::nullopt;
    }

    do {
        if (executable_name == entry.szExeFile) {
            return entry.th32ProcessID;
        }
    } while (Process32NextW(snapshot.get(), &entry));

    return std::nullopt;
}

[[nodiscard]] UniqueHandle CreateModuleSnapshot(DWORD process_id) {
    constexpr DWORD flags = TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32;

    for (int attempt = 0; attempt < 20; ++attempt) {
        HANDLE snapshot = CreateToolhelp32Snapshot(flags, process_id);
        if (snapshot != INVALID_HANDLE_VALUE) {
            return UniqueHandle(snapshot);
        }

        if (GetLastError() != ERROR_BAD_LENGTH) {
            break;
        }

        Sleep(10);
    }

    return {};
}

[[nodiscard]] std::optional<std::uintptr_t>
FindModuleBase(DWORD process_id, std::wstring_view module_name) {
    UniqueHandle snapshot = CreateModuleSnapshot(process_id);
    if (!snapshot) {
        return std::nullopt;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (!Module32FirstW(snapshot.get(), &entry)) {
        return std::nullopt;
    }

    do {
        if (module_name == entry.szModule) {
            return reinterpret_cast<std::uintptr_t>(entry.modBaseAddr);
        }
    } while (Module32NextW(snapshot.get(), &entry));

    return std::nullopt;
}

template <typename T>
[[nodiscard]] std::optional<T>
ReadValue(HANDLE process, std::uintptr_t address) {
    T value{};
    SIZE_T bytes_read = 0;

    if (!ReadProcessMemory(
            process,
            reinterpret_cast<LPCVOID>(address),
            &value,
            sizeof(value),
            &bytes_read) ||
        bytes_read != sizeof(value)) {
        return std::nullopt;
    }

    return value;
}

template <typename T>
[[nodiscard]] bool
WriteProtectedValue(
    HANDLE process,
    std::uintptr_t address,
    const T& value) {
    MEMORY_BASIC_INFORMATION mbi{};

    if (VirtualQueryEx(
            process,
            reinterpret_cast<LPCVOID>(address),
            &mbi,
            sizeof(mbi)) == 0) {
        return false;
    }

    const auto region_start =
        reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
    const std::uint64_t region_end =
        static_cast<std::uint64_t>(region_start) +
        static_cast<std::uint64_t>(mbi.RegionSize);
    const std::uint64_t write_end =
        static_cast<std::uint64_t>(address) + sizeof(T);
    const DWORD protection_base = mbi.Protect & 0xFFU;

    if (mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 ||
        protection_base == PAGE_NOACCESS ||
        address < region_start ||
        write_end > region_end) {
        SetLastError(ERROR_NOACCESS);
        return false;
    }

    const bool executable =
        protection_base == PAGE_EXECUTE ||
        protection_base == PAGE_EXECUTE_READ ||
        protection_base == PAGE_EXECUTE_READWRITE ||
        protection_base == PAGE_EXECUTE_WRITECOPY;
    const DWORD temporary_protection =
        executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;

    DWORD old_protection = 0;
    if (!VirtualProtectEx(
            process,
            reinterpret_cast<LPVOID>(address),
            sizeof(T),
            temporary_protection,
            &old_protection)) {
        return false;
    }

    SIZE_T bytes_written = 0;
    const BOOL write_ok = WriteProcessMemory(
        process,
        reinterpret_cast<LPVOID>(address),
        &value,
        sizeof(value),
        &bytes_written);

    DWORD write_error = ERROR_SUCCESS;
    if (!write_ok) {
        write_error = GetLastError();
    } else if (bytes_written != sizeof(value)) {
        write_error = ERROR_PARTIAL_COPY;
    }

    DWORD ignored = 0;
    const BOOL restore_ok = VirtualProtectEx(
        process,
        reinterpret_cast<LPVOID>(address),
        sizeof(T),
        old_protection,
        &ignored);
    const DWORD restore_error =
        restore_ok ? ERROR_SUCCESS : GetLastError();

    if (write_error != ERROR_SUCCESS) {
        SetLastError(write_error);
        return false;
    }

    if (!restore_ok) {
        SetLastError(restore_error);
        return false;
    }

    return true;
}

[[nodiscard]] bool IsReadableWritableProtection(DWORD protection) {
    const DWORD base = protection & 0xFFU;
    if ((protection & PAGE_GUARD) != 0 || base == PAGE_NOACCESS) {
        return false;
    }

    return base == PAGE_READWRITE ||
           base == PAGE_WRITECOPY ||
           base == PAGE_EXECUTE_READWRITE ||
           base == PAGE_EXECUTE_WRITECOPY;
}

enum class ScanKind {
    Int32,
    Float32
};

enum class FilterKind {
    Changed,
    Unchanged,
    Increased,
    Decreased
};

struct Candidate {
    std::uint32_t address;
    std::uint32_t previous_raw;
};

class UnknownScanner32 {
public:
    static constexpr std::uintptr_t kDefaultLowestAddress = 0x00010000;
    // The client is LARGE_ADDRESS_AWARE, so valid 32-bit user allocations may
    // exist above 0x80000000 on 64-bit Windows.
    static constexpr std::uintptr_t kDefaultHighestAddress = 0xFFF00000;

    bool Start(
        HANDLE process,
        ScanKind kind,
        std::uintptr_t lowest_address = kDefaultLowestAddress,
        std::uintptr_t highest_address = kDefaultHighestAddress,
        std::optional<std::uint32_t> relative_base = std::nullopt) {
        candidates_.clear();
        kind_ = kind;
        truncated_ = false;
        relative_base_ = relative_base;

        lowest_address =
            std::max(lowest_address, kDefaultLowestAddress);
        highest_address =
            std::min(highest_address, kDefaultHighestAddress);

        if (lowest_address >= highest_address) {
            std::cout
                << "Invalid scan range. The first address must be below "
                   "the second address.\n";
            return false;
        }

        lowest_address_ = lowest_address;
        highest_address_ = highest_address;

        constexpr std::size_t kChunkSize = 1024 * 1024;
        constexpr std::size_t kCandidateLimit = 8'000'000;

        std::vector<std::byte> buffer(kChunkSize);
        std::uintptr_t address = lowest_address;

        while (address < highest_address) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQueryEx(
                    process,
                    reinterpret_cast<LPCVOID>(address),
                    &mbi,
                    sizeof(mbi)) == 0) {
                address += 0x1000;
                continue;
            }

            const auto region_base =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const std::uint64_t queried_region_end64 =
                static_cast<std::uint64_t>(region_base) +
                static_cast<std::uint64_t>(mbi.RegionSize);
            const auto queried_region_end =
                queried_region_end64 >= highest_address
                    ? highest_address
                    : static_cast<std::uintptr_t>(
                          queried_region_end64);
            const auto scan_region_start =
                std::max(region_base, lowest_address);
            const auto scan_region_end =
                std::min(queried_region_end, highest_address);

            if (mbi.State == MEM_COMMIT &&
                mbi.Type == MEM_PRIVATE &&
                IsReadableWritableProtection(mbi.Protect)) {
                for (std::uintptr_t chunk_base = scan_region_start;
                     chunk_base < scan_region_end;
                     chunk_base += kChunkSize) {
                    const auto remaining = scan_region_end - chunk_base;
                    const SIZE_T requested =
                        static_cast<SIZE_T>(
                            std::min<std::uintptr_t>(
                                remaining, kChunkSize));

                    SIZE_T bytes_read = 0;
                    if (!ReadProcessMemory(
                            process,
                            reinterpret_cast<LPCVOID>(chunk_base),
                            buffer.data(),
                            requested,
                            &bytes_read)) {
                        continue;
                    }

                    const SIZE_T first_offset =
                        static_cast<SIZE_T>(
                            (sizeof(std::uint32_t) -
                             (chunk_base % sizeof(std::uint32_t))) %
                            sizeof(std::uint32_t));

                    for (SIZE_T offset = first_offset;
                         offset + sizeof(std::uint32_t) <= bytes_read;
                         offset += sizeof(std::uint32_t)) {
                        std::uint32_t raw = 0;
                        std::memcpy(
                            &raw,
                            buffer.data() + offset,
                            sizeof(raw));

                        if (kind_ == ScanKind::Float32) {
                            float value = 0.0F;
                            std::memcpy(&value, &raw, sizeof(value));
                            if (!std::isfinite(value)) {
                                continue;
                            }
                        }

                        const auto candidate_address =
                            chunk_base + offset;

                        if (candidate_address >
                            (std::numeric_limits<std::uint32_t>::max)()) {
                            continue;
                        }

                        candidates_.push_back(
                            Candidate{
                                static_cast<std::uint32_t>(
                                    candidate_address),
                                raw
                            });

                        if (candidates_.size() >= kCandidateLimit) {
                            truncated_ = true;
                            std::cout
                                << "Candidate limit reached ("
                                << kCandidateLimit
                                << ") at 0x"
                                << std::hex << std::uppercase
                                << candidate_address
                                << std::dec
                                << ". Restart with a narrower range.\n";
                            return true;
                        }
                    }
                }
            }

            address = queried_region_end > address
                          ? queried_region_end
                          : address + 0x1000;
        }

        return true;
    }

    bool Filter(HANDLE process, FilterKind filter) {
        if (candidates_.empty()) {
            std::cout << "No active scan.\n";
            return false;
        }

        constexpr std::size_t kChunkSize = 1024 * 1024;
        std::vector<std::byte> buffer(kChunkSize);
        std::vector<Candidate> filtered;
        filtered.reserve(candidates_.size() / 2 + 1);

        std::size_t index = 0;

        while (index < candidates_.size()) {
            const std::uintptr_t candidate_address =
                candidates_[index].address;

            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQueryEx(
                    process,
                    reinterpret_cast<LPCVOID>(candidate_address),
                    &mbi,
                    sizeof(mbi)) == 0 ||
                mbi.State != MEM_COMMIT ||
                !IsReadableWritableProtection(mbi.Protect)) {
                ++index;
                continue;
            }

            const auto region_base =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
            const auto region_end =
                region_base + static_cast<std::uintptr_t>(mbi.RegionSize);

            const auto chunk_base =
                std::max(
                    region_base,
                    candidate_address -
                        (candidate_address % kChunkSize));

            const auto chunk_end =
                std::min(
                    region_end,
                    chunk_base + kChunkSize);

            const SIZE_T requested =
                static_cast<SIZE_T>(chunk_end - chunk_base);
            SIZE_T bytes_read = 0;

            const bool read_ok = ReadProcessMemory(
                process,
                reinterpret_cast<LPCVOID>(chunk_base),
                buffer.data(),
                requested,
                &bytes_read) != FALSE;

            while (index < candidates_.size()) {
                const auto address = candidates_[index].address;
                if (address < chunk_base || address >= chunk_end) {
                    break;
                }

                if (read_ok) {
                    const auto offset =
                        static_cast<SIZE_T>(address - chunk_base);

                    if (offset + sizeof(std::uint32_t) <= bytes_read) {
                        std::uint32_t current_raw = 0;
                        std::memcpy(
                            &current_raw,
                            buffer.data() + offset,
                            sizeof(current_raw));

                        if (Matches(
                                candidates_[index].previous_raw,
                                current_raw,
                                filter)) {
                            filtered.push_back(
                                Candidate{
                                    candidates_[index].address,
                                    current_raw
                                });
                        }
                    }
                }

                ++index;
            }
        }

        candidates_.swap(filtered);
        return true;
    }

    void Show(std::size_t count) const {
        count = std::min(count, candidates_.size());

        for (std::size_t i = 0; i < count; ++i) {
            const auto& candidate = candidates_[i];

            std::int32_t integer_value = 0;
            float float_value = 0.0F;
            std::memcpy(
                &integer_value,
                &candidate.previous_raw,
                sizeof(integer_value));
            std::memcpy(
                &float_value,
                &candidate.previous_raw,
                sizeof(float_value));

            std::cout
                << '[' << i << "] 0x"
                << std::hex << std::uppercase
                << std::setw(8) << std::setfill('0')
                << candidate.address
                << " raw=0x"
                << std::setw(8)
                << candidate.previous_raw
                << std::dec << std::setfill(' ');

            if (relative_base_ &&
                candidate.address >= *relative_base_) {
                std::cout
                    << " (+0x"
                    << std::hex << std::uppercase
                    << candidate.address - *relative_base_
                    << std::dec << ')';
            }

            std::cout
                << " int=" << integer_value
                << " float=" << float_value
                << '\n';
        }

        std::cout << "Total candidates: "
                  << candidates_.size() << '\n';
    }

    void ShowRange() const {
        std::cout
            << "Requested range: [0x"
            << std::hex << std::uppercase
            << lowest_address_
            << ", 0x" << highest_address_
            << ")\n";

        if (candidates_.empty()) {
            std::cout << "Captured range: no candidates\n";
        } else {
            std::cout
                << "Captured candidates: 0x"
                << candidates_.front().address
                << " through 0x"
                << candidates_.back().address
                << '\n';
        }

        std::cout
            << "Candidate limit reached: "
            << std::dec
            << (truncated_ ? "yes" : "no")
            << '\n';
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return candidates_.size();
    }

private:
    [[nodiscard]] bool Matches(
        std::uint32_t old_raw,
        std::uint32_t current_raw,
        FilterKind filter) const {
        if (filter == FilterKind::Changed) {
            return old_raw != current_raw;
        }

        if (filter == FilterKind::Unchanged) {
            return old_raw == current_raw;
        }

        if (kind_ == ScanKind::Int32) {
            std::int32_t old_value = 0;
            std::int32_t current_value = 0;
            std::memcpy(&old_value, &old_raw, sizeof(old_value));
            std::memcpy(
                &current_value,
                &current_raw,
                sizeof(current_value));

            return filter == FilterKind::Increased
                       ? current_value > old_value
                       : current_value < old_value;
        }

        float old_value = 0.0F;
        float current_value = 0.0F;
        std::memcpy(&old_value, &old_raw, sizeof(old_value));
        std::memcpy(
            &current_value,
            &current_raw,
            sizeof(current_value));

        if (!std::isfinite(old_value) ||
            !std::isfinite(current_value)) {
            return false;
        }

        return filter == FilterKind::Increased
                   ? current_value > old_value
                   : current_value < old_value;
    }

    ScanKind kind_ = ScanKind::Int32;
    std::uintptr_t lowest_address_ = kDefaultLowestAddress;
    std::uintptr_t highest_address_ = kDefaultHighestAddress;
    bool truncated_ = false;
    std::optional<std::uint32_t> relative_base_;
    std::vector<Candidate> candidates_;
};

struct SharedConnection {
    UniqueHandle mapping;
    UniqueView view;

    [[nodiscard]] pwnie::SharedState* state() const noexcept {
        return static_cast<pwnie::SharedState*>(view.get());
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return mapping && view;
    }
};

[[nodiscard]] SharedConnection OpenSharedState(DWORD process_id) {
    SharedConnection result;

    std::wstring mapping_name = pwnie::kSharedMappingPrefix;
    mapping_name += std::to_wstring(process_id);

    result.mapping.reset(
        OpenFileMappingW(
            FILE_MAP_ALL_ACCESS,
            FALSE,
            mapping_name.c_str()));

    if (!result.mapping) {
        return result;
    }

    result.view.reset(
        MapViewOfFile(
            result.mapping.get(),
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            sizeof(pwnie::SharedState)));

    if (!result.view) {
        result.mapping.reset();
        return result;
    }

    if (result.state()->magic != pwnie::kSharedMagic ||
        result.state()->version != pwnie::kSharedVersion ||
        result.state()->owner_process_id != process_id) {
        result.view.reset();
        result.mapping.reset();
    }

    return result;
}

[[nodiscard]] bool InjectResearchDll(
    HANDLE process,
    DWORD process_id,
    const std::filesystem::path& input_path) {
    std::error_code ec;
    const auto absolute_path =
        std::filesystem::absolute(input_path, ec);

    if (ec || !std::filesystem::exists(absolute_path)) {
        std::wcerr << L"DLL path does not exist: "
                   << input_path.wstring() << L'\n';
        return false;
    }

    const std::wstring module_name =
        absolute_path.filename().wstring();
    const auto already_loaded =
        FindModuleBase(process_id, module_name);
    if (already_loaded) {
        std::wcerr
            << module_name
            << L" is already loaded at 0x"
            << std::hex << std::uppercase
            << *already_loaded << std::dec
            << L". Use shutdown-dll and wait for it to unload before "
               L"injecting a rebuilt DLL.\n";
        return false;
    }

    const std::wstring path = absolute_path.wstring();
    const SIZE_T byte_count =
        (path.size() + 1) * sizeof(wchar_t);

    void* remote_path = VirtualAllocEx(
        process,
        nullptr,
        byte_count,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE);

    if (remote_path == nullptr) {
        std::wcerr << L"VirtualAllocEx failed. Error: "
                   << GetLastError() << L'\n';
        return false;
    }

    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(
            process,
            remote_path,
            path.c_str(),
            byte_count,
            &bytes_written) ||
        bytes_written != byte_count) {
        std::wcerr << L"Writing DLL path failed. Error: "
                   << GetLastError() << L'\n';
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    HMODULE local_kernel32 =
        GetModuleHandleW(L"kernel32.dll");
    FARPROC local_load_library =
        GetProcAddress(local_kernel32, "LoadLibraryW");

    const auto remote_kernel32 =
        FindModuleBase(process_id, L"KERNEL32.DLL");

    if (local_kernel32 == nullptr ||
        local_load_library == nullptr ||
        !remote_kernel32) {
        std::wcerr << L"Unable to resolve LoadLibraryW.\n";
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const auto load_library_rva =
        reinterpret_cast<std::uintptr_t>(local_load_library) -
        reinterpret_cast<std::uintptr_t>(local_kernel32);

    const auto remote_load_library =
        *remote_kernel32 + load_library_rva;

    UniqueHandle thread(
        CreateRemoteThread(
            process,
            nullptr,
            0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(
                remote_load_library),
            remote_path,
            0,
            nullptr));

    if (!thread) {
        std::wcerr << L"CreateRemoteThread failed. Error: "
                   << GetLastError() << L'\n';
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait_result =
        WaitForSingleObject(thread.get(), 5000);

    if (wait_result != WAIT_OBJECT_0) {
        if (wait_result == WAIT_TIMEOUT) {
            std::wcerr
                << L"LoadLibraryW timed out in the target. "
                   L"The remote path allocation was retained because the "
                   L"thread may still be using it.\n";
        } else {
            std::wcerr << L"Waiting for LoadLibraryW failed. Error: "
                       << GetLastError() << L'\n';
        }
        return false;
    }

    DWORD remote_module = 0;
    if (!GetExitCodeThread(thread.get(), &remote_module)) {
        const DWORD error = GetLastError();
        VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);
        std::wcerr << L"GetExitCodeThread failed. Error: "
                   << error << L'\n';
        return false;
    }

    VirtualFreeEx(process, remote_path, 0, MEM_RELEASE);

    if (remote_module == 0) {
        std::wcerr << L"LoadLibraryW returned NULL in the target.\n";
        return false;
    }

    std::wcout << L"Injected module handle: 0x"
               << std::hex << std::uppercase
               << remote_module << std::dec << L'\n';
    return true;
}

[[nodiscard]] std::optional<std::uint32_t>
ParseOffset(const std::string& text) {
    try {
        std::size_t consumed = 0;
        const unsigned long value =
            std::stoul(text, &consumed, 0);

        if (consumed != text.size() ||
            value >
                (std::numeric_limits<std::uint32_t>::max)()) {
            return std::nullopt;
        }

        return static_cast<std::uint32_t>(value);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<ScanKind>
ParseScanKind(std::string_view text) {
    if (text == "int") {
        return ScanKind::Int32;
    }
    if (text == "float") {
        return ScanKind::Float32;
    }
    return std::nullopt;
}

void ShowNearbyFloats(
    HANDLE process,
    std::uint32_t center,
    std::uint32_t radius) {
    const std::uint32_t start =
        (center > radius ? center - radius : 0U) & ~0x3U;
    const std::uint64_t requested_end =
        static_cast<std::uint64_t>(center) + radius;
    const std::uint32_t end =
        static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                requested_end,
                (std::numeric_limits<std::uint32_t>::max)() - 3ULL)) &
        ~0x3U;

    for (std::uint32_t address = start;
         address <= end;
         address += sizeof(float)) {
        const auto value = ReadValue<float>(process, address);
        if (value && std::isfinite(*value)) {
            std::cout
                << "0x"
                << std::hex << std::uppercase
                << std::setw(8) << std::setfill('0')
                << address
                << std::dec << std::setfill(' ')
                << " = " << *value
                << '\n';
        }

        if (address >
            (std::numeric_limits<std::uint32_t>::max)() -
                sizeof(float)) {
            break;
        }
    }
}

[[nodiscard]] bool IsValidWorldCoordinate(float value) {
    return std::isfinite(value) && std::fabs(value) <= 200000.0F;
}

[[nodiscard]] LONG AtomicRead(volatile LONG* value) {
    return InterlockedCompareExchange(value, 0, 0);
}

[[nodiscard]] bool IsTickFresh(const pwnie::SharedState& state) {
    if (state.tick_hook_ready != 1 || state.tick_count == 0) {
        return false;
    }

    const DWORD last_tick = static_cast<DWORD>(state.last_tick_ms);
    return GetTickCount() - last_tick <= 2000;
}

[[nodiscard]] bool IsValidFlyWalkingSpeed(float value) {
    return std::isfinite(value) && value >= 0.0F && value <= 20000.0F;
}

[[nodiscard]] bool IsValidFlyJumpSpeed(float value) {
    return std::isfinite(value) && value >= 10.0F && value <= 20000.0F;
}

[[nodiscard]] bool IsValidFlyHoldTime(float value) {
    return std::isfinite(value) && value >= 0.2F && value <= 999999.0F;
}

[[nodiscard]] bool ReadPositionSnapshot(
    const pwnie::SharedState& state,
    float& x,
    float& y,
    float& z,
    LONG& updates) {
    if (!state.coordinates_valid) {
        return false;
    }

    for (int attempt = 0; attempt < 20; ++attempt) {
        const LONG sequence_before = AtomicRead(
            const_cast<volatile LONG*>(&state.position_sequence));
        if ((sequence_before & 1) != 0) {
            YieldProcessor();
            continue;
        }

        MemoryBarrier();
        const float snapshot_x = state.coordinate_x;
        const float snapshot_y = state.coordinate_y;
        const float snapshot_z = state.coordinate_z;
        const LONG snapshot_updates = state.position_update_count;
        MemoryBarrier();

        const LONG sequence_after = AtomicRead(
            const_cast<volatile LONG*>(&state.position_sequence));
        if (sequence_before == sequence_after &&
            (sequence_after & 1) == 0 &&
            state.coordinates_valid) {
            x = snapshot_x;
            y = snapshot_y;
            z = snapshot_z;
            updates = snapshot_updates;
            return true;
        }
    }
    return false;
}

void PrintPosition(const pwnie::SharedState& state) {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    LONG updates = 0;
    if (!ReadPositionSnapshot(state, x, y, z, updates)) {
        std::cout
            << "Position is not available yet. The Tick hook will publish it "
               "after the local world starts updating.\n";
        return;
    }

    const std::streamsize old_precision = std::cout.precision();
    std::cout << std::setprecision(9)
        << "Position: ("
        << x << ", "
        << y << ", "
        << z << ")"
        << "\nPosition updates: "
        << updates << '\n';
    std::cout.precision(old_precision);
}

void PrintMovement(const pwnie::SharedState& state) {
    if (!state.movement_valid) {
        std::cout
            << "Movement fields are not available yet. Wait for a local "
               "Player::Tick.\n";
        return;
    }

    MemoryBarrier();
    std::cout
        << "Current movement fields:"
        << "\n  walking speed: " << state.movement_walking_speed
        << "\n  jump speed: " << state.movement_jump_speed
        << "\n  jump hold time: " << state.movement_jump_hold_time
        << '\n';
}

void PrintFlyStatus(const pwnie::SharedState& state) {
    MemoryBarrier();
    std::cout
        << "Fly enabled: " << state.fly_enabled
        << "\nConfigured walking speed: " << state.fly_walking_speed
        << "\nConfigured jump speed: " << state.fly_jump_speed
        << "\nConfigured jump hold time: " << state.fly_jump_hold_time
        << "\nCanJump patch active: " << state.can_jump_patch_active
        << "\nMovement snapshot valid: " << state.movement_snapshot_valid
        << "\nFly updates: " << state.fly_update_count
        << "\nFly restores: " << state.fly_restore_count
        << "\nFly error: " << state.fly_error
        << '\n';

    PrintMovement(state);
}

[[nodiscard]] bool WaitForFlyRestore(
    pwnie::SharedState* state,
    DWORD timeout_ms) {
    const DWORD start = GetTickCount();

    while (state->can_jump_patch_active != 0 ||
           state->movement_snapshot_valid != 0) {
        if (GetTickCount() - start >= timeout_ms) {
            return false;
        }
        Sleep(10);
    }

    MemoryBarrier();
    return true;
}

[[nodiscard]] bool QueueTeleport(
    pwnie::SharedState* state,
    float x,
    float y,
    float z) {
    if (!IsValidWorldCoordinate(x) ||
        !IsValidWorldCoordinate(y) ||
        !IsValidWorldCoordinate(z)) {
        std::cout
            << "Each coordinate must be finite and within "
               "[-200000, 200000].\n";
        return false;
    }

    if (state->hook_ready != 1 || !IsTickFresh(*state)) {
        std::cout
            << "A fresh local Player::Tick was not observed; teleport was "
               "not queued. Resume the game world and try again.\n";
        return false;
    }

    if (state->player_address == 0) {
        std::cout
            << "The local Player is not available yet. Wait for a Tick and "
               "try again.\n";
        return false;
    }

    // request_id is also a seqlock: even values are published requests and
    // odd values mean a producer is writing the tuple.  Only one outstanding
    // request is allowed, so two controller processes cannot mix coordinates.
    const LONG previous_request = AtomicRead(&state->teleport_request_id);
    if ((previous_request & 1) != 0 ||
        previous_request != AtomicRead(&state->teleport_completed_id)) {
        std::cout
            << "Another teleport request is being published or is still "
               "pending. Wait for it to complete.\n";
        return false;
    }

    if (previous_request > (std::numeric_limits<LONG>::max)() - 2 ||
        InterlockedCompareExchange(
            &state->teleport_request_id,
            previous_request + 1,
            previous_request) != previous_request) {
        std::cout
            << "Another controller acquired the teleport mailbox; retry.\n";
        return false;
    }

    state->teleport_x = x;
    state->teleport_y = y;
    state->teleport_z = z;
    MemoryBarrier();

    const LONG request_id = previous_request + 2;
    InterlockedExchange(&state->teleport_request_id, request_id);

    const DWORD start = GetTickCount();
    while (AtomicRead(&state->teleport_completed_id) != request_id) {
        if (GetTickCount() - start >= 2000) {
            std::cout
                << "Teleport request " << request_id
                << " timed out after 2 seconds. The game may be paused or "
                   "not ticking.\n";
            return false;
        }
        Sleep(10);
    }

    MemoryBarrier();
    if (state->teleport_succeeded != 1) {
        std::cout
            << "Teleport request " << request_id
            << " was processed but Actor::SetPosition failed.\n";
        return false;
    }

    std::cout
        << "Teleport request " << request_id
        << " completed on the game thread.\n";
    PrintPosition(*state);
    return true;
}

void PrintHelp() {
    std::cout << R"HELP(
Commands
--------
help
status
read-sprint
write-sprint <float>

scan-start int|float [lowestAddress highestAddress]
player-scan-start int|float [size, default 0x400]
scan-filter changed
scan-filter unchanged
scan-filter increased
scan-filter decreased
scan-show [count]
scan-range
show-floats <address> [radius, default 0x20]

inject <absolute-or-relative-path-to-pwnie_research.dll>

player
capture-player
position
teleport <x> <y> <z>
teleport-up <delta>
movement
fly status
fly on
fly off
fly speed <jumpSpeed, 10..20000>
fly hold <seconds, 0.2..999999>
fly walk <walkingSpeed, 0..20000>
call-internal-sprint
hook-sprint on <float>
hook-sprint off
shutdown-dll

quit

Notes
-----
1. Run the game and enter the local world before starting this tool.
2. Build both the EXE and DLL as 32-bit.
3. The Player::Tick hook automatically identifies the local complete Player*;
   no sprint input is required for capture.
4. Health is read from complete Player + 0x30 for the verified local build.
5. Scans include writable MEM_PRIVATE pages only; address ranges are [low, high).
6. Position and teleport use Actor::GetPosition/SetPosition on the game thread;
   direct guessed coordinate offsets are deliberately not used.
7. fly off restores the captured movement values and original CanJump bytes.
)HELP";
}

} // namespace

int main() {
    static_assert(
        sizeof(void*) == 4,
        "Build this project as 32-bit because the target client is 32-bit.");

    const auto process_id =
        FindProcessId(pwnie::kProcessName);

    if (!process_id) {
        std::wcerr << L"Process not found: "
                   << pwnie::kProcessName << L'\n';
        return 1;
    }

    UniqueHandle process(
        OpenProcess(
            PROCESS_QUERY_INFORMATION |
                PROCESS_VM_READ |
                PROCESS_VM_WRITE |
                PROCESS_VM_OPERATION |
                PROCESS_CREATE_THREAD |
                SYNCHRONIZE,
            FALSE,
            *process_id));

    if (!process) {
        std::wcerr << L"OpenProcess failed. Error: "
                   << GetLastError() << L'\n';
        return 1;
    }

    const auto game_logic_base =
        FindModuleBase(
            *process_id,
            pwnie::kGameLogicName);

    if (!game_logic_base) {
        std::wcerr << L"GameLogic.dll is not loaded yet.\n";
        return 1;
    }

    std::cout << "Attached to PID " << *process_id
              << ", GameLogic base 0x"
              << std::hex << std::uppercase
              << *game_logic_base
              << std::dec << "\n";

    UnknownScanner32 scanner;
    PrintHelp();

    std::string line;

    while (std::cout << "\npwnie> " &&
           std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        input >> command;

        if (command.empty()) {
            continue;
        }

        if (command == "quit" || command == "exit") {
            break;
        }

        if (command == "help") {
            PrintHelp();
            continue;
        }

        if (command == "status") {
            std::cout << "PID: " << *process_id
                      << "\nGameLogic base: 0x"
                      << std::hex << std::uppercase
                      << *game_logic_base << std::dec
                      << "\nScanner candidates: "
                      << scanner.size() << '\n';

            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Injected DLL state: not available\n";
            } else {
                const auto* state = shared.state();
                MemoryBarrier();
                std::cout
                    << "Hook ready: "
                    << state->hook_ready
                    << "\nInitialization error: "
                    << state->init_error
                    << "\nSignatures verified: "
                    << state->signatures_verified
                    << "\nTick hook ready: "
                    << state->tick_hook_ready
                    << "\nSprint hook ready: "
                    << state->sprint_hook_ready
                    << "\nPlayer: 0x"
                    << std::hex << std::uppercase
                    << state->player_address
                    << std::dec
                    << "\nPlayer capture armed: "
                    << state->capture_player_requested
                    << "\nTick count: "
                    << state->tick_count
                    << "\nLast Tick time (GetTickCount ms): "
                    << state->last_tick_ms
                    << "\nFresh local Tick (<=2s): "
                    << (IsTickFresh(*state) ? 1 : 0)
                    << "\nHealth (+0x30): "
                    << state->health
                    << "\nHealth valid: "
                    << state->health_valid
                    << "\nCoordinates valid: "
                    << state->coordinates_valid
                    << "\nPosition updates: "
                    << state->position_update_count
                    << "\nMovement valid: "
                    << state->movement_valid
                    << "\nTeleport request/completed: "
                    << state->teleport_request_id << '/'
                    << state->teleport_completed_id
                    << "\nLast teleport succeeded: "
                    << state->teleport_succeeded
                    << "\nFly enabled: "
                    << state->fly_enabled
                    << "\nCanJump patch active: "
                    << state->can_jump_patch_active
                    << "\nMovement snapshot valid: "
                    << state->movement_snapshot_valid
                    << "\nFly updates/restores/error: "
                    << state->fly_update_count << '/'
                    << state->fly_restore_count << '/'
                    << state->fly_error
                    << "\nInternal sprint result: "
                    << state->last_internal_sprint_result
                    << '\n';

                if (state->coordinates_valid) {
                    PrintPosition(*state);
                }
                if (state->movement_valid) {
                    PrintMovement(*state);
                }
            }
            continue;
        }

        if (command == "read-sprint") {
            const auto address =
                *game_logic_base +
                pwnie::kSprintMultiplierDataRva;

            const auto value =
                ReadValue<float>(process.get(), address);

            if (!value) {
                std::cout << "Read failed. Error: "
                          << GetLastError() << '\n';
            } else {
                std::uint32_t raw = 0;
                std::memcpy(&raw, &*value, sizeof(raw));

                std::cout << "Address: 0x"
                          << std::hex << std::uppercase
                          << address
                          << "\nRaw: 0x"
                          << std::setw(8)
                          << std::setfill('0')
                          << raw
                          << std::dec << std::setfill(' ')
                          << "\nFloat: " << *value << '\n';
            }
            continue;
        }

        if (command == "write-sprint") {
            float value = 0.0F;
            if (!(input >> value) ||
                !std::isfinite(value) ||
                value <= 0.0F ||
                value > 1000.0F) {
                std::cout
                    << "Enter a finite value in (0, 1000].\n";
                continue;
            }

            const auto address =
                *game_logic_base +
                pwnie::kSprintMultiplierDataRva;

            if (!WriteProtectedValue(process.get(), address, value)) {
                std::cout << "Write failed. Error: "
                          << GetLastError() << '\n';
            } else {
                const auto verified =
                    ReadValue<float>(process.get(), address);

                if (!verified ||
                    std::memcmp(
                        &*verified,
                        &value,
                        sizeof(value)) != 0) {
                    std::cout
                        << "Write completed, but read-back verification failed.\n";
                } else {
                    std::cout << "Sprint constant changed to "
                              << value
                              << " (verified)\n";
                }
            }
            continue;
        }

        if (command == "scan-start") {
            std::string kind_text;
            input >> kind_text;
            const auto kind = ParseScanKind(kind_text);

            if (!kind) {
                std::cout
                    << "Use: scan-start int|float [lowest highest]\n";
                continue;
            }

            std::uintptr_t lowest =
                UnknownScanner32::kDefaultLowestAddress;
            std::uintptr_t highest =
                UnknownScanner32::kDefaultHighestAddress;

            std::string lowest_text;
            if (input >> lowest_text) {
                std::string highest_text;
                if (!(input >> highest_text)) {
                    std::cout
                        << "Supply both range endpoints: "
                           "scan-start int|float <lowest> <highest>\n";
                    continue;
                }

                const auto parsed_lowest = ParseOffset(lowest_text);
                const auto parsed_highest = ParseOffset(highest_text);
                if (!parsed_lowest || !parsed_highest) {
                    std::cout
                        << "Invalid 32-bit address range.\n";
                    continue;
                }

                lowest = *parsed_lowest;
                highest = *parsed_highest;
            }

            std::cout
                << (*kind == ScanKind::Float32
                        ? "Capturing finite float candidates...\n"
                        : "Capturing 32-bit integer candidates...\n");

            if (!scanner.Start(
                    process.get(),
                    *kind,
                    lowest,
                    highest)) {
                continue;
            }

            std::cout << "Initial candidates: "
                      << scanner.size() << '\n';
            scanner.ShowRange();
            continue;
        }

        if (command == "player-scan-start") {
            std::string kind_text;
            input >> kind_text;
            const auto kind = ParseScanKind(kind_text);
            if (!kind) {
                std::cout
                    << "Use: player-scan-start int|float [size]\n";
                continue;
            }

            std::uint32_t size = 0x400;
            std::string size_text;
            if (input >> size_text) {
                const auto parsed_size = ParseOffset(size_text);
                if (!parsed_size ||
                    *parsed_size < sizeof(std::uint32_t) ||
                    *parsed_size > 0x00100000) {
                    std::cout
                        << "Size must be between 4 and 0x100000 bytes.\n";
                    continue;
                }
                size = *parsed_size;
            }

            auto shared = OpenSharedState(*process_id);
            if (!shared || shared.state()->player_address == 0) {
                std::cout
                    << "Player is unavailable. Inject the DLL and wait for "
                       "the local Player::Tick hook to capture it.\n";
                continue;
            }

            const std::uint32_t player =
                shared.state()->player_address;
            const std::uint64_t end64 =
                static_cast<std::uint64_t>(player) + size;
            if (end64 >
                (std::numeric_limits<std::uint32_t>::max)()) {
                std::cout << "Player scan range overflows 32-bit space.\n";
                continue;
            }

            if (!scanner.Start(
                    process.get(),
                    *kind,
                    player,
                    static_cast<std::uintptr_t>(end64),
                    player)) {
                continue;
            }

            std::cout
                << "Captured " << scanner.size()
                << " Player-relative candidates. Change the game state, "
                   "then use scan-filter.\n";
            scanner.ShowRange();
            continue;
        }

        if (command == "scan-filter") {
            std::string filter_text;
            input >> filter_text;

            std::optional<FilterKind> filter;
            if (filter_text == "changed") {
                filter = FilterKind::Changed;
            } else if (filter_text == "unchanged") {
                filter = FilterKind::Unchanged;
            } else if (filter_text == "increased") {
                filter = FilterKind::Increased;
            } else if (filter_text == "decreased") {
                filter = FilterKind::Decreased;
            }

            if (!filter) {
                std::cout
                    << "Use changed|unchanged|increased|decreased\n";
                continue;
            }

            scanner.Filter(process.get(), *filter);
            std::cout << "Remaining candidates: "
                      << scanner.size() << '\n';
            continue;
        }

        if (command == "scan-show") {
            std::size_t count = 25;
            input >> count;
            scanner.Show(count);
            continue;
        }

        if (command == "scan-range") {
            scanner.ShowRange();
            continue;
        }

        if (command == "show-floats") {
            std::string address_text;
            std::string radius_text;
            input >> address_text;

            const auto address = ParseOffset(address_text);
            std::uint32_t radius = 0x20;

            if (input >> radius_text) {
                const auto parsed_radius = ParseOffset(radius_text);
                if (!parsed_radius || *parsed_radius > 0x10000) {
                    std::cout
                        << "Radius must be between 0 and 0x10000.\n";
                    continue;
                }
                radius = *parsed_radius;
            }

            if (!address) {
                std::cout
                    << "Use: show-floats <address> [radius]\n";
                continue;
            }

            ShowNearbyFloats(process.get(), *address, radius);
            continue;
        }

        if (command == "inject") {
            std::string path_text;
            std::getline(input >> std::ws, path_text);

            if (path_text.empty()) {
                std::cout << "Use: inject <DLL path>\n";
                continue;
            }

            if (!InjectResearchDll(
                    process.get(),
                    *process_id,
                    std::filesystem::path(path_text))) {
                continue;
            }

            bool hook_ready = false;
            bool state_seen = false;
            LONG init_error = 0;
            LONG signatures_verified = 0;
            LONG tick_hook_ready = 0;
            LONG sprint_hook_ready = 0;
            for (int attempt = 0; attempt < 200; ++attempt) {
                auto shared = OpenSharedState(*process_id);
                if (shared) {
                    state_seen = true;
                    const auto* state = shared.state();
                    MemoryBarrier();
                    init_error = state->init_error;
                    signatures_verified = state->signatures_verified;
                    tick_hook_ready = state->tick_hook_ready;
                    sprint_hook_ready = state->sprint_hook_ready;

                    if (state->hook_ready == 1) {
                        hook_ready = true;
                        break;
                    }

                    if (init_error != 0) {
                        break;
                    }
                }
                Sleep(10);
            }

            if (hook_ready) {
                std::cout
                    << "Research DLL initialized. Signature checks passed; "
                       "Tick and sprint hooks are ready.\n";
            } else if (state_seen) {
                std::cout
                    << "DLL state appeared, but initialization did not "
                       "complete."
                    << "\n  init_error: " << init_error
                    << "\n  signatures_verified: " << signatures_verified
                    << "\n  tick_hook_ready: " << tick_hook_ready
                    << "\n  sprint_hook_ready: " << sprint_hook_ready
                    << "\nDo not use runtime commands until status reports "
                       "Hook ready: 1. Verify this GameLogic.dll build and "
                       "its RVAs.\n";
            } else {
                std::cout
                    << "DLL loaded, but no compatible shared state appeared "
                       "within 2 seconds. Check DLL/EXE protocol versions and "
                       "DLL initialization.\n";
            }
            continue;
        }

        if (command == "capture-player") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->capture_player_requested),
                0);
            InterlockedExchange(
                reinterpret_cast<volatile LONG*>(
                    &state->player_address),
                0);
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->health_valid),
                0);
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->coordinates_valid),
                0);
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->movement_valid),
                0);
            MemoryBarrier();
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->capture_player_requested),
                1);

            std::cout
                << "Player state cleared and automatic recapture armed. "
                   "The next verified local Player::Tick will capture it; "
                   "no sprint input is required.\n";
            continue;
        }

        if (command == "player") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout
                    << "Research DLL is not injected or initialized.\n";
                continue;
            }

            auto* state = shared.state();
            const std::uintptr_t player =
                state->player_address;

            if (player == 0) {
                std::cout
                    << "Player not captured yet. Enter the local world and "
                       "wait for Player::Tick, or run capture-player to rearm.\n";
                continue;
            }

            std::optional<std::int32_t> health;
            if (pwnie::kPlayerHealthOffset <=
                (std::numeric_limits<std::uintptr_t>::max)() - player) {
                health = ReadValue<std::int32_t>(
                    process.get(),
                    player + pwnie::kPlayerHealthOffset);
            }

            std::cout << "Player*: 0x"
                      << std::hex << std::uppercase
                      << player << std::dec
                      << "\nTick count: " << state->tick_count
                      << "\nLast Tick time (GetTickCount ms): "
                      << state->last_tick_ms << '\n';

            if (!IsTickFresh(*state)) {
                std::cout
                    << "Warning: this Player snapshot is stale; resume the "
                       "local world before issuing runtime requests.\n";
            }

            if (health) {
                std::cout
                    << "Health (+0x30, external read): "
                    << *health
                    << "\nHealth (Tick snapshot): "
                    << state->health
                    << " (valid=" << state->health_valid << ")\n";
                if (*health != state->health && state->health_valid) {
                    std::cout
                        << "The external read and Tick snapshot differ; run "
                           "player again to rule out an in-flight update.\n";
                }
            } else {
                std::cout
                    << "Health field (+0x30) read failed.\n";
            }

            PrintPosition(*state);
            PrintMovement(*state);
            continue;
        }

        if (command == "position") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            PrintPosition(*shared.state());
            continue;
        }

        if (command == "teleport") {
            float x = 0.0F;
            float y = 0.0F;
            float z = 0.0F;

            if (!(input >> x >> y >> z)) {
                std::cout << "Use: teleport <x> <y> <z>\n";
                continue;
            }

            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            const bool teleport_completed =
                QueueTeleport(shared.state(), x, y, z);
            (void)teleport_completed;
            continue;
        }

        if (command == "teleport-up") {
            float delta = 0.0F;
            if (!(input >> delta) ||
                !std::isfinite(delta) ||
                std::fabs(delta) > 200000.0F) {
                std::cout
                    << "Use: teleport-up <finite delta within "
                       "[-200000, 200000]>\n";
                continue;
            }

            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();
            float x = 0.0F;
            float y = 0.0F;
            float current_z = 0.0F;
            LONG position_updates = 0;
            if (!ReadPositionSnapshot(
                    *state,
                    x,
                    y,
                    current_z,
                    position_updates)) {
                std::cout
                    << "Current position is unavailable; teleport-up was "
                       "not queued.\n";
                continue;
            }

            const float z = current_z + delta;
            const bool teleport_completed =
                QueueTeleport(state, x, y, z);
            (void)teleport_completed;
            continue;
        }

        if (command == "movement") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            PrintMovement(*shared.state());
            continue;
        }

        if (command == "fly") {
            std::string mode;
            input >> mode;

            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();

            if (mode == "status") {
                PrintFlyStatus(*state);
                continue;
            }

            if (mode == "on") {
                if (state->hook_ready != 1 ||
                    !IsTickFresh(*state) ||
                    state->player_address == 0) {
                    std::cout
                        << "Fly was not enabled: a fresh local Player::Tick "
                           "and Player address are required.\n";
                    continue;
                }

                float walking_speed = state->fly_walking_speed;
                float jump_speed = state->fly_jump_speed;
                float hold_time = state->fly_jump_hold_time;

                if (!IsValidFlyWalkingSpeed(walking_speed)) {
                    walking_speed = 800.0F;
                }
                if (!IsValidFlyJumpSpeed(jump_speed)) {
                    jump_speed = 999.0F;
                }
                if (!IsValidFlyHoldTime(hold_time)) {
                    hold_time = 99999.0F;
                }

                state->fly_walking_speed = walking_speed;
                state->fly_jump_speed = jump_speed;
                state->fly_jump_hold_time = hold_time;
                MemoryBarrier();

                const LONG old_update_count = state->fly_update_count;
                InterlockedExchange(
                    const_cast<LONG*>(&state->fly_enabled),
                    1);

                const DWORD start = GetTickCount();
                while (state->can_jump_patch_active == 0 &&
                       state->fly_error == 0 &&
                       state->fly_update_count == old_update_count &&
                       GetTickCount() - start < 1500) {
                    Sleep(10);
                }

                if (state->can_jump_patch_active &&
                    state->fly_update_count != old_update_count &&
                    state->fly_error == 0) {
                    std::cout
                        << "Fly enabled on the game thread; movement fields "
                           "and CanJump patch are active.\n";
                } else if (state->fly_error != 0) {
                    std::cout
                        << "Fly enable failed. Error code: "
                        << state->fly_error << '\n';
                } else {
                    std::cout
                        << "Fly enable is queued, but no active game Tick was "
                           "observed within 1.5 seconds.\n";
                }
                PrintFlyStatus(*state);
                continue;
            }

            if (mode == "off") {
                const LONG old_restore_count = state->fly_restore_count;
                InterlockedExchange(
                    const_cast<LONG*>(&state->fly_enabled),
                    0);

                if (WaitForFlyRestore(state, 1500)) {
                    std::cout
                        << "Fly disabled; original CanJump bytes and captured "
                           "movement values are restored."
                        << " Restore count: " << state->fly_restore_count
                        << " (was " << old_restore_count << ").\n";
                } else {
                    std::cout
                        << "Fly disable was requested, but restoration was not "
                           "observed within 1.5 seconds. The game may be "
                           "paused; status will show pending state.\n";
                }
                continue;
            }

            if (mode == "speed") {
                float value = 0.0F;
                if (!(input >> value) || !IsValidFlyJumpSpeed(value)) {
                    std::cout
                        << "Use: fly speed <finite jump speed from 10 to "
                           "20000>\n";
                    continue;
                }
                state->fly_jump_speed = value;
                MemoryBarrier();
                std::cout << "Configured fly jump speed: " << value << '\n';
                continue;
            }

            if (mode == "hold") {
                float value = 0.0F;
                if (!(input >> value) || !IsValidFlyHoldTime(value)) {
                    std::cout
                        << "Use: fly hold <finite value from 0.2 to "
                           "999999>\n";
                    continue;
                }
                state->fly_jump_hold_time = value;
                MemoryBarrier();
                std::cout
                    << "Configured fly jump hold time: " << value << '\n';
                continue;
            }

            if (mode == "walk") {
                float value = 0.0F;
                if (!(input >> value) || !IsValidFlyWalkingSpeed(value)) {
                    std::cout
                        << "Use: fly walk <finite walking speed from 0 to "
                           "20000>\n";
                    continue;
                }
                state->fly_walking_speed = value;
                MemoryBarrier();
                std::cout
                    << "Configured fly walking speed: " << value << '\n';
                continue;
            }

            std::cout
                << "Use: fly status|on|off|speed <N>|hold <N>|walk <N>\n";
            continue;
        }

        if (command == "set-coords" || command == "clear-coords") {
            std::cout
                << "This command is deprecated. Direct coordinate offsets "
                   "were removed; use position, teleport, or teleport-up, "
                   "which call verified Actor methods on the game thread.\n";
            continue;
        }

        if (command == "call-internal-sprint") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();
            if (state->player_address == 0) {
                std::cout
                    << "Player not captured yet. Wait for the local Tick "
                       "hook; no sprint input is required.\n";
                continue;
            }

            if (!IsTickFresh(*state)) {
                std::cout
                    << "A fresh Tick is unavailable; the internal call "
                       "cannot be scheduled safely on the game thread.\n";
                continue;
            }

            const LONG old_count =
                state->internal_sprint_call_count;

            InterlockedExchange(
                const_cast<LONG*>(
                    &state->call_internal_sprint_requested),
                1);

            for (int i = 0; i < 200; ++i) {
                if (state->internal_sprint_call_count != old_count) {
                    break;
                }
                Sleep(10);
            }

            if (state->internal_sprint_call_count == old_count) {
                std::cout
                    << "Internal call timed out after 2 seconds; the game "
                       "may be paused or not ticking.\n";
            } else {
                std::cout
                    << "Original Player::GetSprintMultiplier() returned "
                    << state->last_internal_sprint_result << '\n';
            }
            continue;
        }

        if (command == "hook-sprint") {
            std::string mode;
            input >> mode;

            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();

            if (mode == "off") {
                InterlockedExchange(
                    const_cast<LONG*>(
                        &state->sprint_override_enabled),
                    0);
                std::cout << "Sprint hook override disabled.\n";
                continue;
            }

            if (mode == "on") {
                float value = 0.0F;
                if (!(input >> value) ||
                    !std::isfinite(value) ||
                    value <= 0.0F ||
                    value > 1000.0F) {
                    std::cout
                        << "Use: hook-sprint on <value in (0,1000]>\n";
                    continue;
                }

                state->sprint_override_value = value;
                InterlockedExchange(
                    const_cast<LONG*>(
                        &state->sprint_override_enabled),
                    1);
                std::cout << "Hook will return "
                          << value << '\n';
                continue;
            }

            std::cout << "Use: hook-sprint on <value>|off\n";
            continue;
        }

        if (command == "shutdown-dll") {
            auto shared = OpenSharedState(*process_id);
            if (!shared) {
                std::cout << "Research DLL state is unavailable.\n";
                continue;
            }

            auto* state = shared.state();
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->fly_enabled),
                0);
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->sprint_override_enabled),
                0);

            if (WaitForFlyRestore(state, 1500)) {
                std::cout
                    << "Fly state is inactive and runtime patches are "
                       "restored.\n";
            } else {
                std::cout
                    << "Fly restoration was not observed within 1.5 seconds; "
                       "shutdown cleanup will make one final restoration "
                       "attempt.\n";
            }

            MemoryBarrier();
            InterlockedExchange(
                const_cast<LONG*>(
                    &state->shutdown_requested),
                1);
            std::cout
                << "Shutdown requested after disabling runtime overrides.\n";
            continue;
        }

        std::cout << "Unknown command. Type help.\n";
    }

    return 0;
}
