#pragma once

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "adapter.h"
#include "compare.h"

namespace dwarf_parser_check {

struct ResolveReport {
  std::vector<KernelResolution> resolutions;
  std::vector<ComparisonReport> comparisons;
  std::vector<std::string> diagnostics;

  [[nodiscard]] bool empty() const noexcept {
    for (const KernelResolution& resolution : resolutions) {
      if (!resolution.locations.empty()) {
        return false;
      }
    }

    return true;
  }
};

class AdapterRegistry {
 public:
  void register_adapter(DwarfAdapterPtr adapter);

  [[nodiscard]] const std::vector<DwarfAdapterPtr>& adapters() const noexcept;

  [[nodiscard]] std::vector<const DwarfAdapter*> matching_adapters(const ResolveRequest& request) const;

 private:
  std::vector<DwarfAdapterPtr> adapters_;
};

class ResolverEngine {
 public:
  explicit ResolverEngine(AdapterRegistry registry);

  [[nodiscard]] const AdapterRegistry& registry() const noexcept;

  [[nodiscard]] ResolveReport resolve(const ResolveRequest& request) const;

 private:
  AdapterRegistry registry_;
};

AdapterRegistry make_registry(std::vector<DwarfAdapterPtr> adapters);

std::vector<std::string> compiled_adapter_names();

std::vector<DwarfAdapterPtr> create_adapters(std::string_view selection);

ResolveReport resolve_request(
    const ResolverEngine& engine,
  const ResolveRequest& request);

}  // namespace dwarf_parser_check