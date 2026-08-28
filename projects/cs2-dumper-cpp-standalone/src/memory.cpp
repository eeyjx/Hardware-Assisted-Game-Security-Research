#include "cs2dumper/memory.hpp"

#include <iomanip>
#include <sstream>

namespace cs2dumper {

std::string IProcessMemory::hex(std::uint64_t value) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << value;
    return ss.str();
}

std::unique_ptr<IProcessMemory> create_native_win32_process(const MemoryBackendOptions& options);

std::unique_ptr<IProcessMemory> create_process_memory(const MemoryBackendOptions& options) {
    return create_native_win32_process(options);
}

} // namespace cs2dumper
