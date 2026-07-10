#include "cli.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace dwarf_parser_check {
namespace {

std::optional<std::uint64_t> parse_ip(std::string_view value) {
  std::size_t parsed = 0;
  const int base = value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0 ? 16 : 10;
  const std::uint64_t ip = std::stoull(std::string(value), &parsed, base);
  if (parsed != value.size()) {
    return std::nullopt;
  }
  return ip;
}

std::string path_kind_label(PathKind kind) {
  switch (kind) {
    case PathKind::kUser:
      return "user";
    case PathKind::kSystem:
      return "system";
    case PathKind::kUnknown:
    default:
      return "unknown";
  }
}

void print_location(const SourceLocation& location, std::ostream& output) {
  output << "  - 0x" << std::hex << location.location.ip << std::dec << " -> "
         << location.location.file;

  if (location.location.line.has_value()) {
    output << ':' << *location.location.line;
  }

  if (location.location.column.has_value()) {
    output << ':' << *location.location.column;
  }

  output << " [" << path_kind_label(location.path_kind) << "]\n";

  for (const InlineFrame& frame : location.inline_chain) {
    output << "    inline: " << frame.function_name << " at " << frame.file;
    if (frame.line.has_value()) {
      output << ':' << *frame.line;
    }
    if (frame.column.has_value()) {
      output << ':' << *frame.column;
    }
    output << '\n';
  }

  for (const std::string& note : location.backend_notes) {
    output << "    note: " << note << '\n';
  }
}

}  // namespace

std::optional<CliOptions> parse_cli(int argc, char** argv, std::ostream& error_stream) {
  CliOptions options;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);

    if (argument == "--help" || argument == "-h") {
      options.show_help = true;
      return options;
    }

    auto require_value = [&](std::string_view flag) -> std::optional<std::string_view> {
      if (index + 1 >= argc) {
        error_stream << "missing value for " << flag << '\n';
        return std::nullopt;
      }
      ++index;
      return std::string_view(argv[index]);
    };

    if (argument == "--dwarf-file") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.request.dwarf_file = *value;
      continue;
    }

    if (argument == "--kernel") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.request.kernel_name = std::string(*value);
      continue;
    }

    if (argument == "--ip") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      try {
        const auto ip = parse_ip(*value);
        if (!ip.has_value()) {
          error_stream << "invalid IP value: " << *value << '\n';
          return std::nullopt;
        }
        options.request.ips.push_back(*ip);
      } catch (const std::exception&) {
        error_stream << "invalid IP value: " << *value << '\n';
        return std::nullopt;
      }
      continue;
    }

    if (argument == "--all-ips") {
      options.request.resolve_all_ips = true;
      continue;
    }

    if (argument == "--reference") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.request.reference_file = std::filesystem::path(*value);
      continue;
    }

    if (argument == "--project-root") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.request.project_root = std::filesystem::path(*value);
      continue;
    }

    if (argument == "--first-only") {
      options.resolve_options.stop_after_first_success = true;
      continue;
    }

    error_stream << "unknown argument: " << argument << '\n';
    return std::nullopt;
  }

  if (options.request.dwarf_file.empty()) {
    error_stream << "--dwarf-file is required\n";
    return std::nullopt;
  }

  if (options.request.kernel_name.empty()) {
    error_stream << "--kernel is required\n";
    return std::nullopt;
  }

  if (options.request.ips.empty() && !options.request.resolve_all_ips) {
    error_stream << "provide at least one --ip or use --all-ips\n";
    return std::nullopt;
  }

  return options;
}

void print_usage(std::ostream& output, const char* program_name) {
  output << "Usage: " << program_name << " --dwarf-file <PATH> --kernel <NAME> [options]\n"
         << "\n"
         << "Options:\n"
         << "  --dwarf-file <PATH>   Path to compressed DWARF file\n"
         << "  --kernel <NAME>       Kernel name to resolve\n"
         << "  --ip <HEX_OR_DEC>     Instruction pointer to resolve, repeatable\n"
         << "  --all-ips             Resolve all available IPs\n"
         << "  --reference <PATH>    Optional VTune reference file\n"
         << "  --project-root <PATH> Optional root used to prefer user paths\n"
         << "  --first-only          Stop after the first successful adapter\n"
         << "  --help, -h            Show this help message\n";
}

void print_report(const ResolveReport& report, std::ostream& output) {
  for (const KernelResolution& resolution : report.resolutions) {
    output << "backend: " << resolution.backend_name << '\n';
    output << "kernel: " << resolution.kernel_name << '\n';

    for (const std::string& warning : resolution.warnings) {
      output << "warning: " << warning << '\n';
    }

    for (const SourceLocation& location : resolution.locations) {
      print_location(location, output);
    }

    output << '\n';
  }

  if (report.comparison.has_value()) {
    output << "comparison: " << report.comparison->mismatch_count() << " mismatch(es)";
    output << (report.comparison->has_mismatches() ? " found" : ", all matched") << '\n';
  }

  for (const std::string& diagnostic : report.diagnostics) {
    output << "diagnostic: " << diagnostic << '\n';
  }
}

}  // namespace dwarf_parser_check