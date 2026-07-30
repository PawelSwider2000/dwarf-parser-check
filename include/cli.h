#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

namespace dwarf_parser_check {

struct ResolveReport;

struct CliOptions {
  std::filesystem::path kernel_debug_json;
  std::optional<std::filesystem::path> reference_file;
  std::optional<std::filesystem::path> vtune_manifest;
  std::filesystem::path output_dir;
  std::string adapter_selection = "all";
  bool show_help = false;
};

std::optional<CliOptions> parse_cli(
    int argc,
    char** argv,
    std::ostream& error_stream);

void print_usage(std::ostream& output, const char* program_name);

void print_report(const ResolveReport& report, std::ostream& output);

void write_report_csv(const ResolveReport& report, std::ostream& output);

void write_report_json(const ResolveReport& report, std::ostream& output);

}  // namespace dwarf_parser_check