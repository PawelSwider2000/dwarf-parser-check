#include "core.h"

#include <stdexcept>

#include "compare.h"
#include "dummy_adapter.h"

namespace dwarf_parser_check {

void AdapterRegistry::register_adapter(DwarfAdapterPtr adapter) {
  adapters_.push_back(std::move(adapter));
}

const std::vector<DwarfAdapterPtr>& AdapterRegistry::adapters() const noexcept {
  return adapters_;
}

std::vector<const DwarfAdapter*> AdapterRegistry::matching_adapters(const ResolveRequest& request) const {
  std::vector<const DwarfAdapter*> matches;
  for (const auto& adapter : adapters_) {
    if (adapter != nullptr && adapter->supports(request)) {
      matches.push_back(adapter.get());
    }
  }
  return matches;
}

ResolverEngine::ResolverEngine(AdapterRegistry registry) : registry_(std::move(registry)) {}

const AdapterRegistry& ResolverEngine::registry() const noexcept {
  return registry_;
}

ResolveReport ResolverEngine::resolve(const ResolveRequest& request, const ResolveOptions& options) const {
  ResolveReport report;
  const auto matches = registry_.matching_adapters(request);

  for (const DwarfAdapter* adapter : matches) {
    if (adapter == nullptr) {
      continue;
    }

    report.resolutions.push_back(adapter->resolve_kernel(request));

    if (options.stop_after_first_success && !report.resolutions.empty()) {
      break;
    }
  }

  if (report.resolutions.empty()) {
    report.diagnostics.push_back("No adapter produced a resolution for the request.");
  }

  return report;
}

AdapterRegistry make_default_registry() {
  AdapterRegistry registry;
  registry.register_adapter(make_dummy_adapter());
  return registry;
}

ResolveReport resolve_request(
    const ResolverEngine& engine,
    const ResolveRequest& request,
    const ResolveOptions& options) {
  ResolveReport report = engine.resolve(request, options);

  if (options.compare_with_reference && request.reference_file.has_value() && !report.resolutions.empty()) {
    const std::vector<Location> references = load_reference_locations(*request.reference_file, request.kernel_name);
    report.comparison = compare_locations(report.resolutions.front(), references);
  }

  return report;
}

}  // namespace dwarf_parser_check