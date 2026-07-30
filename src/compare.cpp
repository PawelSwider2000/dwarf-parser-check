#include "compare.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
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

std::vector<std::string> parse_csv_row(std::string_view row) {
  std::vector<std::string> fields;
  std::string field;
  bool quoted = false;
  for (std::size_t index = 0; index < row.size(); ++index) {
    const char character = row[index];
    if (quoted) {
      if (character == '"' && index + 1U < row.size() && row[index + 1U] == '"') {
        field += '"';
        ++index;
      } else if (character == '"') {
        quoted = false;
      } else {
        field += character;
      }
    } else if (character == '"') {
      quoted = true;
    } else if (character == ',') {
      fields.push_back(std::move(field));
      field.clear();
    } else if (character != '\r') {
      field += character;
    }
  }
  if (quoted) {
    throw std::runtime_error("unterminated quoted field in VTune CSV reference");
  }
  fields.push_back(std::move(field));
  return fields;
}

std::vector<Location> load_vtune_reference_csv(
    const std::string& contents,
    const std::string& kernel_name) {
  std::istringstream input(contents);
  std::string line;
  if (!std::getline(input, line)) {
    throw std::runtime_error("VTune CSV reference is empty");
  }

  const std::vector<std::string> header = parse_csv_row(line);
  const auto find_column = [&header](std::string_view name) -> std::optional<std::size_t> {
    const auto column = std::find(header.begin(), header.end(), name);
    if (column == header.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(std::distance(header.begin(), column));
  };
  const auto address_col_a = find_column("Address");
  const auto address_column = address_col_a.has_value() ? address_col_a : find_column("Kernel Offset");
  const auto file_column = find_column("Source File");
  const auto line_column = find_column("Source Line");
  if (!address_column.has_value() || !file_column.has_value() || !line_column.has_value()) {
    throw std::runtime_error("VTune CSV reference must contain Address or Kernel Offset, Source File, and Source Line columns");
  }

  std::vector<Location> locations;
  while (std::getline(input, line)) {
    const std::vector<std::string> row = parse_csv_row(line);
    const std::size_t required_column = std::max({*address_column, *file_column, *line_column});
    if (row.size() <= required_column || row[*file_column].empty() || row[*line_column].empty()) {
      continue;
    }

    Location location;
    location.kernel_name = kernel_name;
    try {
      location.ip = std::stoull(row[*address_column], nullptr, 0);
      location.line = std::stoull(row[*line_column]);
    } catch (const std::exception&) {
      throw std::runtime_error("invalid address or source line in VTune CSV reference");
    }
    location.file = row[*file_column];
    locations.push_back(std::move(location));
  }
  if (locations.empty()) {
    throw std::runtime_error("VTune CSV reference contains no source locations");
  }
  return locations;
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
  std::ifstream input(reference_file, std::ios::binary);
  if (!input.is_open()) {
    throw std::runtime_error("Failed to open reference file: " + reference_file.string());
  }

  const std::string contents((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  const std::size_t first_line_end = contents.find_first_of("\r\n");
  const std::vector<std::string> header = parse_csv_row(contents.substr(0, first_line_end));
  if (std::find(header.begin(), header.end(), "Address") != header.end() ||
      std::find(header.begin(), header.end(), "Kernel Offset") != header.end()) {
    return load_vtune_reference_csv(contents, kernel_name);
  }

  std::vector<Location> locations;
  std::istringstream input_stream(contents);
  std::string line;
  while (std::getline(input_stream, line)) {
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
  std::vector<std::uint64_t> compared_ips;

  for (const Location& reference : reference_locations) {
    if (std::find(compared_ips.begin(), compared_ips.end(), reference.ip) != compared_ips.end()) {
      continue;
    }
    compared_ips.push_back(reference.ip);

    std::vector<Location> candidates;
    for (const Location& candidate : reference_locations) {
      if (candidate.ip == reference.ip) {
        candidates.push_back(candidate);
      }
    }

    const auto resolved_it = std::find_if(
        resolution.locations.begin(),
        resolution.locations.end(),
        [&reference](const SourceLocation& location) {
          return location.location.ip == reference.ip;
        });
    if (resolved_it == resolution.locations.end()) {
      ComparisonItem item;
      item.status = ComparisonStatus::kMissingInBackend;
      item.reference = candidates.front();
      item.notes.push_back("reference location is missing in backend results");
      report.items.push_back(std::move(item));
      continue;
    }

    const std::size_t resolved_index =
        static_cast<std::size_t>(std::distance(resolution.locations.begin(), resolved_it));
    matched_resolved[resolved_index] = true;
    std::vector<ComparisonItem> candidate_items;
    candidate_items.reserve(candidates.size());
    for (const Location& candidate : candidates) {
      candidate_items.push_back(compare_pair(*resolved_it, candidate, options));
    }

    const auto matched_candidate = std::find_if(
        candidate_items.begin(),
        candidate_items.end(),
        [](const ComparisonItem& item) {
          return item.status == ComparisonStatus::kMatch;
        });
    ComparisonItem item = matched_candidate != candidate_items.end()
        ? *matched_candidate
        : candidate_items.front();
    if (candidates.size() > 1U) {
      item.notes.push_back("reference contains multiple source locations for this offset");
    }
    report.items.push_back(std::move(item));
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