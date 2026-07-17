#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace dwarf_parser_check {

struct ResolveReport;

struct CliOptions {
  std::filesystem::path kernel_debug_json;
  std::vector<std::uint64_t> ips;
  bool resolve_all_ips = false;
  std::optional<std::filesystem::path> reference_file;
  std::string adapter_selection = "all";
  bool show_help = false;
};

std::optional<CliOptions> parse_cli(
    int argc,
    char** argv,
    std::ostream& error_stream);

void print_usage(std::ostream& output, const char* program_name);

void print_report(const ResolveReport& report, std::ostream& output);

}  // namespace dwarf_parser_check