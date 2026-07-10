#pragma once

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "adapter.h"
#include "compare.h"

namespace dwarf_parser_check {

struct ResolveOptions {
  bool stop_after_first_success = false;
  bool compare_with_reference = true;
};

struct ResolveReport {
  std::vector<KernelResolution> resolutions;
  std::optional<ComparisonReport> comparison;
  std::vector<std::string> diagnostics;

  [[nodiscard]] bool empty() const noexcept {
    return resolutions.empty();
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

  [[nodiscard]] ResolveReport resolve(const ResolveRequest& request, const ResolveOptions& options = {}) const;

 private:
  AdapterRegistry registry_;
};

AdapterRegistry make_default_registry();

ResolveReport resolve_request(
    const ResolverEngine& engine,
    const ResolveRequest& request,
    const ResolveOptions& options = {});

}  // namespace dwarf_parser_check