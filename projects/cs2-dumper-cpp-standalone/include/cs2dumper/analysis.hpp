#pragma once

#include "cs2dumper/memory.hpp"
#include "cs2dumper/runtime_config.hpp"
#include "cs2dumper/types.hpp"

namespace cs2dumper {

ButtonMap analyze_buttons(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout);
InterfaceMap analyze_interfaces(IProcessMemory& process, const LayoutConfig& layout);
OffsetMap analyze_offsets(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout);
SchemaMap analyze_schemas(IProcessMemory& process, const SignatureDatabase& signatures,
                          const LayoutConfig& layout);
AnalysisResult analyze_all(IProcessMemory& process, const SignatureDatabase& signatures,
                           const LayoutConfig& layout);

} // namespace cs2dumper
