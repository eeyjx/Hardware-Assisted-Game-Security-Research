#pragma once

#include "cs2dumper/memory.hpp"
#include "cs2dumper/types.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace cs2dumper {

class Output {
public:
    Output(std::vector<std::string> file_types,
           std::size_t indent_size,
           std::filesystem::path out_dir,
           const AnalysisResult& result);

    void dump_all(IProcessMemory& process) const;

private:
    std::vector<std::string> file_types_;
    std::size_t indent_size_{};
    std::filesystem::path out_dir_;
    const AnalysisResult& result_;
    std::string timestamp_rfc3339_;
    std::string timestamp_banner_;
};

std::string slugify(std::string input);

} // namespace cs2dumper
