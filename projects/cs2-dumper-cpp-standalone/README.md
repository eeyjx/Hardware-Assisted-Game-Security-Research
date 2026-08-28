# cs2-dumper-cpp-standalone

A self-contained C++20 implementation for inspecting a running Source 2 process on Windows and producing C++ headers plus JSON data.

The executable has no network requirement and does not download signatures or layouts from any repository at runtime. The process reader uses Windows APIs directly.

## Design goals

The project is designed so that ordinary game updates do not require editing the C++ source code:

- `CreateInterface` is used to discover registered Source 2 interfaces.
- `SchemaSystem_001` is the primary route into the schema system, avoiding a version-specific schema signature when the interface remains available.
- machine-code signatures are stored in `signatures.cfg` and support multiple candidates for the same value;
- Source 2 structure offsets are stored in `layout.cfg`;
- both configuration files are copied beside the executable after a CMake build;
- external configuration entries are tried before built-in fallbacks;
- failed signatures do not stop the rest of the analysis pipeline;
- `info.json` can still be produced when the build-number signature is unavailable.

This significantly reduces maintenance, but no binary-analysis tool can guarantee permanent compatibility with arbitrary future engine changes. If instruction sequences change, update `signatures.cfg`. If internal schema layouts shift, update `layout.cfg`. A fundamental engine/API redesign can still require C++ changes.

## Source and output language

Implementation source files are C++ (`.cpp` / `.hpp`) with CMake build files. Generated code is C++ header output (`.hpp`); JSON is available as a data format. No other source-language generator is included.

## Build with Visual Studio 2022

Open **Developer PowerShell for VS 2022**:

```powershell
cd C:\path\to\cs2_dumper_cpp_standalone
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build copies these files beside the executable:

```text
build\Release\cs2-dumper.exe
build\Release\signatures.cfg
build\Release\layout.cfg
```

## Run

Start the target process first, then:

```powershell
.\build\Release\cs2-dumper.exe -p cs2.exe -o output -vv
```

Default generated files include:

```text
output\buttons.hpp
output\buttons.json
output\interfaces.hpp
output\interfaces.json
output\offsets.hpp
output\offsets.json
output\<module>.hpp
output\<module>.json
output\info.json
```

## Runtime configuration

The executable searches for `signatures.cfg` and `layout.cfg` in this order:

1. an explicit `--signatures` / `--layout` path;
2. the executable directory;
3. the current working directory;
4. the current working directory's `config` folder.

If neither file is found, built-in defaults are used.

### `signatures.cfg`

Format:

```text
module|name|kind|callback|pattern
```

`kind` is `rva` or `imm`. `callback` is `none`, `view_angles`, or `local_player_pawn`.

The same module/name may appear more than once. Candidates are tried in order, so a new signature can be added without removing an older fallback.

Example:

```text
client.dll|dwEntityList|rva|none|48890d${'} e9${} cc
client.dll|dwEntityList|rva|none|488935${'} 4885f6
```

### `layout.cfg`

Format:

```text
key=value
```

Values may be decimal or hexadecimal. Example:

```text
schema_system.type_scopes=0x190
type_scope.class_bindings=0x560
class.fields=0x30
```

If a future engine build shifts only these structure members, updating this file is enough; recompilation is not required.

## CLI

```text
-f, --file-types <TYPES>      comma-separated: hpp,json
-i, --indent-size <N>         indentation width (default 4)
-o, --output <PATH>           output directory (default output)
-p, --process-name <NAME>     process name (default cs2.exe)
-s, --signatures <PATH>       signature override file
-l, --layout <PATH>           Source 2 layout override file
-v, --verbose                 repeat to increase verbosity
-n, --no-log-file             do not create cs2-dumper.log
-V, --version
-h, --help
```

## Project layout

```text
config/
  signatures.cfg             hot-updatable signature candidates
  layout.cfg                 hot-updatable Source 2 ABI offsets
include/cs2dumper/
  analysis.hpp               analysis API
  memory.hpp                 Windows process-memory abstraction
  output.hpp                 C++/JSON output API
  pattern.hpp                pattern parser/scanner
  pe.hpp                     in-memory PE parser
  runtime_config.hpp         signature/layout configuration
  types.hpp                  result/data types
src/
  main.cpp                   CLI and orchestration
  analysis.cpp               buttons/interfaces/offsets/schemas
  memory.cpp                 process-memory factory
  memory_win32.cpp           Windows process reader
  output.cpp                 C++ header and JSON generators
  pattern.cpp                pattern scanner
  pe.cpp                     PE implementation
  runtime_config.cpp         configuration parser/defaults
tests/
  pattern_tests.cpp
  output_tests.cpp
  config_tests.cpp
```

## Compatibility strategy

The analyzer deliberately separates three layers:

1. **Interface discovery** — derived from PE exports and Source 2's registered interface list.
2. **Instruction signatures** — data-driven and replaceable at runtime through `signatures.cfg`.
3. **Object layouts** — data-driven and replaceable at runtime through `layout.cfg`.

This means normal binary drift can usually be handled by replacing data files rather than rebuilding the executable.

