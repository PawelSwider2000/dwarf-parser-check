#pragma once

#include <iosfwd>
#include <optional>
#include <string>

#include "core.h"

namespace dwarf_parser_check {

struct CliOptions {
  ResolveRequest request;
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