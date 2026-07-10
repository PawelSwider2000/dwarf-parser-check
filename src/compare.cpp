#include "compare.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace dwarf_parser_check {
namespace {

std::string normalize_path(const std::string& input, bool normalize) {
  if (!normalize) {
    return input;
  }

  return std::filesystem::path(input).lexically_normal().generic_string();
}

ComparisonItem compare_pair(
    const SourceLocation& resolved,
    const Location& reference,
    const ComparisonOptions& options) {
  ComparisonItem item;
  item.resolved = resolved;
  item.reference = reference;

  const std::string resolved_path = normalize_path(resolved.location.file, options.normalize_paths);
  const std::string reference_path = normalize_path(reference.file, options.normalize_paths);

  if (resolved_path != reference_path) {
    item.status = ComparisonStatus::kFileMismatch;
    item.notes.push_back("resolved file differs from reference file");
    return item;
  }

  if (resolved.location.line != reference.line) {
    item.status = ComparisonStatus::kLineMismatch;
    item.notes.push_back("resolved line differs from reference line");
    return item;
  }

  if (options.compare_columns && resolved.location.column != reference.column) {
    item.status = ComparisonStatus::kColumnMismatch;
    item.notes.push_back("resolved column differs from reference column");
    return item;
  }

  item.notes.push_back("resolved location matches reference");
  return item;
}

}  // namespace

std::vector<Location> load_reference_locations(
    const std::filesystem::path& reference_file,
    const std::string& kernel_name) {
  std::ifstream input(reference_file);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open reference file: " + reference_file.string());
  }

  std::vector<Location> locations;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }

    std::istringstream stream(line);
    Location location;
    std::string ip_text;
    std::uint64_t line_number = 0;
    std::uint64_t column_number = 0;

    if (!(stream >> location.kernel_name >> ip_text >> location.file >> line_number)) {
      throw std::runtime_error("Invalid reference line: " + line);
    }

    const int base = ip_text.rfind("0x", 0) == 0 || ip_text.rfind("0X", 0) == 0 ? 16 : 10;
    location.ip = std::stoull(ip_text, nullptr, base);
    location.line = line_number;

    if (stream >> column_number) {
      location.column = column_number;
    }

    if (location.kernel_name == kernel_name) {
      locations.push_back(std::move(location));
    }
  }

  return locations;
}

ComparisonReport compare_locations(
    const KernelResolution& resolution,
    const std::vector<Location>& reference_locations,
    const ComparisonOptions& options) {
  ComparisonReport report;
  report.backend_name = resolution.backend_name;
  report.kernel_name = resolution.kernel_name;

  std::vector<bool> matched_resolved(resolution.locations.size(), false);

  for (const Location& reference : reference_locations) {
    bool found = false;
    for (std::size_t index = 0; index < resolution.locations.size(); ++index) {
      const SourceLocation& resolved = resolution.locations[index];
      if (resolved.location.ip != reference.ip) {
        continue;
      }

      report.items.push_back(compare_pair(resolved, reference, options));
      matched_resolved[index] = true;
      found = true;
      break;
    }

    if (!found) {
      ComparisonItem item;
      item.status = ComparisonStatus::kMissingInBackend;
      item.reference = reference;
      item.notes.push_back("reference location is missing in backend results");
      report.items.push_back(std::move(item));
    }
  }

  for (std::size_t index = 0; index < resolution.locations.size(); ++index) {
    if (matched_resolved[index]) {
      continue;
    }

    ComparisonItem item;
    item.status = ComparisonStatus::kMissingInReference;
    item.resolved = resolution.locations[index];
    item.notes.push_back("backend returned a location not present in the reference file");
    report.items.push_back(std::move(item));
  }

  return report;
}

}  // namespace dwarf_parser_check