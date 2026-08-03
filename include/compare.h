#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace dwarf_parser_check {

enum class ComparisonStatus {
  kMatch,
  kFileMismatch,
  kLineMismatch,
  kColumnMismatch,
  kMissingInReference,
  kMissingInBackend,
};

struct ComparisonOptions {
  bool normalize_paths = true;
  bool compare_columns = false;
};

struct ComparisonItem {
  ComparisonStatus status = ComparisonStatus::kMatch;
  std::optional<SourceLocation> resolved;
  std::optional<Location> reference;
  std::vector<std::string> notes;
};

struct ComparisonReport {
  std::string backend_name;
  std::string kernel_name;
  std::vector<ComparisonItem> items;
  std::optional<std::string> skip_reason;

  [[nodiscard]] bool is_skipped() const noexcept {
    return skip_reason.has_value();
  }

  [[nodiscard]] bool has_mismatches() const noexcept {
    return std::any_of(items.begin(), items.end(), [](const ComparisonItem& item) {
      return item.status != ComparisonStatus::kMatch;
    });
  }

  [[nodiscard]] std::size_t mismatch_count() const noexcept {
    return static_cast<std::size_t>(std::count_if(items.begin(), items.end(), [](const ComparisonItem& item) {
      return item.status != ComparisonStatus::kMatch;
    }));
  }
};

enum class ReferenceAvailability {
  kAvailable,
  kNoVtuneSourceLocations,
};

struct ReferenceLocations {
  std::vector<Location> locations;
  ReferenceAvailability availability = ReferenceAvailability::kAvailable;
};

ReferenceLocations load_reference_locations(
    const std::filesystem::path& reference_file,
    const std::string& kernel_name);

ComparisonReport compare_locations(
    const KernelResolution& resolution,
    const std::vector<Location>& reference_locations,
    const ComparisonOptions& options = {});

}  // namespace dwarf_parser_check