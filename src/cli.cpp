#include "cli.h"

#include "core.h"

#include <charconv>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace dwarf_parser_check {
namespace {

std::string join_adapter_names(const std::vector<std::string>& names) {
  std::ostringstream stream;
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index != 0U) {
      stream << ", ";
    }
    stream << names[index];
  }
  return stream.str();
}

void write_csv_field(std::string_view value, std::ostream& output) {
  if (value.find_first_of(",\"\r\n") == std::string_view::npos) {
    output << value;
    return;
  }

  output << '"';
  for (const char character : value) {
    if (character == '"') {
      output << '"';
    }
    output << character;
  }
  output << '"';
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

  output << '\n';

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

std::string json_escape(std::string_view value) {
  std::ostringstream escaped;
  for (const char character : value) {
    switch (character) {
      case '\\': escaped << "\\\\"; break;
      case '"': escaped << "\\\""; break;
      case '\b': escaped << "\\b"; break;
      case '\f': escaped << "\\f"; break;
      case '\n': escaped << "\\n"; break;
      case '\r': escaped << "\\r"; break;
      case '\t': escaped << "\\t"; break;
      default:
        if (static_cast<unsigned char>(character) < 0x20U) {
          escaped << "\\u00" << std::hex << std::setw(2) << std::setfill('0')
                  << static_cast<unsigned int>(static_cast<unsigned char>(character))
                  << std::dec << std::setfill(' ');
        } else {
          escaped << character;
        }
    }
  }
  return escaped.str();
}

std::string format_hex(std::uint64_t value) {
  std::ostringstream formatted;
  formatted << "0x" << std::hex << value;
  return formatted.str();
}

const char* comparison_status_name(ComparisonStatus status) {
  switch (status) {
    case ComparisonStatus::kMatch: return "match";
    case ComparisonStatus::kFileMismatch: return "file_mismatch";
    case ComparisonStatus::kLineMismatch: return "line_mismatch";
    case ComparisonStatus::kColumnMismatch: return "column_mismatch";
    case ComparisonStatus::kMissingInReference: return "missing_in_reference";
    case ComparisonStatus::kMissingInBackend: return "missing_in_backend";
  }
  return "unknown";
}

void write_json_string_array(
    const std::vector<std::string>& values,
    std::ostream& output,
    std::size_t indent) {
  if (values.empty()) {
    output << "[]";
    return;
  }

  output << "[\n";
  const std::string value_indent(indent + 2U, ' ');
  for (std::size_t index = 0; index < values.size(); ++index) {
    output << value_indent << '"' << json_escape(values[index]) << '"';
    output << (index + 1U == values.size() ? '\n' : ',') << '\n';
  }
  output << std::string(indent, ' ') << ']';
}

void write_json_hex_array(
    const std::vector<std::uint64_t>& values,
    std::ostream& output,
    std::size_t indent) {
  if (values.empty()) {
    output << "[]";
    return;
  }

  output << "[\n";
  const std::string value_indent(indent + 2U, ' ');
  for (std::size_t index = 0; index < values.size(); ++index) {
    output << value_indent << '"' << format_hex(values[index]) << '"';
    output << (index + 1U == values.size() ? '\n' : ',') << '\n';
  }
  output << std::string(indent, ' ') << ']';
}

void write_location_json(
    const Location& location,
    std::ostream& output,
    std::size_t indent) {
  const std::string field_indent(indent + 2U, ' ');
  output << "{\n"
         << field_indent << "\"kernel\": \"" << json_escape(location.kernel_name) << "\",\n"
         << field_indent << "\"offset\": \"" << format_hex(location.ip) << "\",\n"
         << field_indent << "\"file\": \"" << json_escape(location.file) << "\",\n"
         << field_indent << "\"line\": ";
  if (location.line.has_value()) {
    output << *location.line;
  } else {
    output << "null";
  }
  output << ",\n" << field_indent << "\"column\": ";
  if (location.column.has_value()) {
    output << *location.column;
  } else {
    output << "null";
  }
  output << '\n' << std::string(indent, ' ') << '}';
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

    if (argument == "--kernel-debug-json") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.kernel_debug_json = *value;
      continue;
    }

    if (argument == "--reference") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.reference_file = std::filesystem::path(*value);
      continue;
    }

    if (argument == "--vtune-manifest") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.vtune_manifest = std::filesystem::path(*value);
      continue;
    }

    if (argument == "--output-dir") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.output_dir = std::filesystem::path(*value);
      continue;
    }

    if (argument == "--adapters") {
      const auto value = require_value(argument);
      if (!value.has_value()) {
        return std::nullopt;
      }
      options.adapter_selection = std::string(*value);
      continue;
    }

    error_stream << "unknown argument: " << argument << '\n';
    return std::nullopt;
  }

  if (options.kernel_debug_json.empty()) {
    error_stream << "--kernel-debug-json is required\n";
    return std::nullopt;
  }
  if (options.output_dir.empty()) {
    error_stream << "--output-dir is required\n";
    return std::nullopt;
  }

  return options;
}

void print_usage(std::ostream& output, const char* program_name) {
  const std::vector<std::string> adapters = compiled_adapter_names();
  const std::string adapter_list = adapters.empty() ? "(none)" : join_adapter_names(adapters);

  output << "Usage: " << program_name << " --kernel-debug-json <PATH> [options]\n"
      << "\n"
      << "Options:\n"
        << "  --kernel-debug-json <PATH>  Metadata JSON written by simple_sycl_vtune\n"
         << "  --adapters <LIST>     Comma-separated adapters or 'all' (compiled: " << adapter_list << ")\n"
         << "  --reference <PATH>    Optional reference or VTune source-locations JSON\n"
         << "  --output-dir <PATH>   Write one CSV and comparison JSON per selected adapter\n"
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

  for (const ComparisonReport& comparison : report.comparisons) {
    output << "comparison (" << comparison.backend_name << ", " << comparison.kernel_name
           << "): ";
    if (comparison.is_skipped()) {
      output << "skipped: " << *comparison.skip_reason << '\n';
    } else {
      output << comparison.mismatch_count() << " mismatch(es)";
      output << (comparison.has_mismatches() ? " found" : ", all matched") << '\n';
    }
  }

  for (const std::string& diagnostic : report.diagnostics) {
    output << "diagnostic: " << diagnostic << '\n';
  }
}

void write_report_csv(const ResolveReport& report, std::ostream& output) {
  output << "Kernel Offset,Source File,Source Line\n";
  for (const KernelResolution& resolution : report.resolutions) {
    for (const SourceLocation& source_location : resolution.locations) {
      const Location& location = source_location.location;
      output << "0x" << std::hex << location.ip << std::dec << ',';
      write_csv_field(location.file, output);
      output << ',';
      if (location.line.has_value()) {
        output << *location.line;
      }
      output << '\n';
    }
  }
}

void write_report_json(const ResolveReport& report, std::ostream& output) {
  output << "{\n  \"schema_version\": 2,\n  \"comparisons\": [\n";
  for (std::size_t comparison_index = 0; comparison_index < report.comparisons.size(); ++comparison_index) {
    const ComparisonReport& comparison = report.comparisons[comparison_index];
    std::size_t status_counts[6] = {};
    std::vector<std::uint64_t> matched_ips;
    std::vector<const ComparisonItem*> mismatches;
    for (const ComparisonItem& item : comparison.items) {
      ++status_counts[static_cast<std::size_t>(item.status)];
      if (item.status == ComparisonStatus::kMatch) {
        if (item.resolved.has_value()) {
          matched_ips.push_back(item.resolved->location.ip);
        }
      } else {
        mismatches.push_back(&item);
      }
    }
    output << "    {\n"
           << "      \"backend\": \"" << json_escape(comparison.backend_name) << "\",\n"
           << "      \"kernel\": \"" << json_escape(comparison.kernel_name) << "\",\n"
           << "      \"status\": \"" << (comparison.is_skipped() ? "skipped" : "compared") << "\",\n";
    if (comparison.is_skipped()) {
      output << "      \"skip_reason\": \"" << json_escape(*comparison.skip_reason) << "\",\n";
    }
    output
           << "      \"mismatch_count\": " << comparison.mismatch_count() << ",\n"
           << "      \"summary\": {\n"
           << "        \"compared_offsets\": " << comparison.items.size() << ",\n"
           << "        \"matches\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kMatch)] << ",\n"
           << "        \"file_mismatches\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kFileMismatch)] << ",\n"
           << "        \"line_mismatches\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kLineMismatch)] << ",\n"
           << "        \"column_mismatches\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kColumnMismatch)] << ",\n"
           << "        \"missing_in_reference\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kMissingInReference)] << ",\n"
           << "        \"missing_in_backend\": " << status_counts[static_cast<std::size_t>(ComparisonStatus::kMissingInBackend)] << "\n"
           << "      },\n"
           << "      \"matched_ips\": ";
    write_json_hex_array(matched_ips, output, 6U);
    output << ",\n"
           << "      \"items\": [";
    if (!mismatches.empty()) {
      output << '\n';
    }
    for (std::size_t item_index = 0; item_index < mismatches.size(); ++item_index) {
      const ComparisonItem& item = *mismatches[item_index];
      output << "        {\n"
             << "          \"status\": \"" << comparison_status_name(item.status) << "\",\n"
             << "          \"resolved\": ";
      if (item.resolved.has_value()) {
        write_location_json(item.resolved->location, output, 10U);
      } else {
        output << "null";
      }
      output << ",\n          \"reference\": ";
      if (item.reference.has_value()) {
        write_location_json(*item.reference, output, 10U);
      } else {
        output << "null";
      }
      output << ",\n          \"notes\": ";
      write_json_string_array(item.notes, output, 10U);
      output << "\n        }";
      output << (item_index + 1U == mismatches.size() ? '\n' : ',') << '\n';
    }
    output << "      ]\n    }";
    if (comparison_index + 1U != report.comparisons.size()) output << ',';
    output << '\n';
  }
  output << "  ],\n  \"diagnostics\": ";
  write_json_string_array(report.diagnostics, output, 2U);
  output << "\n}\n";
}

}  // namespace dwarf_parser_check