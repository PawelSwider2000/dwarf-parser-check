#include "core.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

#include "adapters/gdb/gdb_intel_adapter.h"
#include "compare.h"
#include "adapters/gimli/rust_gimli_adapter.h"
#include "adapters/iga/iga_adapter.h"

namespace dwarf_parser_check {

namespace {

struct AdapterDescriptor {
  const char* name;
  DwarfAdapterPtr (*create)();
};

std::vector<AdapterDescriptor> compiled_adapter_descriptors() {
  std::vector<AdapterDescriptor> descriptors;
#if defined(DPC_HAVE_RUST_GIMLI_ADAPTER)
  descriptors.push_back({"rust-gimli", &make_rust_gimli_adapter});
#endif
#if defined(DPC_HAVE_IGA_ADAPTER)
  descriptors.push_back({"iga", &make_iga_adapter});
#endif
#if defined(DPC_HAVE_GDB_INTEL_ADAPTER)
  descriptors.push_back({"gdb-intel", &make_gdb_intel_adapter});
#endif
  return descriptors;
}

std::vector<std::string_view> split_adapter_selection(std::string_view selection) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;

  while (start <= selection.size()) {
    const std::size_t separator = selection.find(',', start);
    const std::size_t end = separator == std::string_view::npos ? selection.size() : separator;
    std::string_view token = selection.substr(start, end - start);
    while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
      token.remove_prefix(1U);
    }
    while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
      token.remove_suffix(1U);
    }
    if (!token.empty()) {
      parts.push_back(token);
    }

    if (separator == std::string_view::npos) {
      break;
    }

    start = separator + 1U;
  }

  return parts;
}

}  // namespace

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

ResolveReport ResolverEngine::resolve(const ResolveRequest& request) const {
  ResolveReport report;
  const auto matches = registry_.matching_adapters(request);

  for (const DwarfAdapter* adapter : matches) {
    if (adapter == nullptr) {
      continue;
    }

    report.resolutions.push_back(adapter->resolve_kernel(request));
  }

  if (report.resolutions.empty()) {
    report.diagnostics.push_back("No adapter produced a resolution for the request.");
  }

  return report;
}

AdapterRegistry make_registry(std::vector<DwarfAdapterPtr> adapters) {
  AdapterRegistry registry;
  for (DwarfAdapterPtr& adapter : adapters) {
    registry.register_adapter(std::move(adapter));
  }

  return registry;
}

std::vector<std::string> compiled_adapter_names() {
  std::vector<std::string> names;
  for (const AdapterDescriptor& descriptor : compiled_adapter_descriptors()) {
    names.push_back(descriptor.name);
  }
  return names;
}

std::vector<DwarfAdapterPtr> create_adapters(std::string_view selection) {
  const std::vector<AdapterDescriptor> descriptors = compiled_adapter_descriptors();
  while (!selection.empty() && (selection.front() == ' ' || selection.front() == '\t')) {
    selection.remove_prefix(1U);
  }
  while (!selection.empty() && (selection.back() == ' ' || selection.back() == '\t')) {
    selection.remove_suffix(1U);
  }

  if (selection.empty()) {
    throw std::invalid_argument("adapter selection must not be empty");
  }

  if (selection == "all") {
    std::vector<DwarfAdapterPtr> adapters;
    adapters.reserve(descriptors.size());
    for (const AdapterDescriptor& descriptor : descriptors) {
      adapters.push_back(descriptor.create());
    }
    return adapters;
  }

  const std::vector<std::string_view> requested = split_adapter_selection(selection);
  if (requested.empty()) {
    throw std::invalid_argument("adapter selection must contain at least one adapter name");
  }

  std::vector<DwarfAdapterPtr> adapters;
  std::vector<std::string> unknown;
  std::vector<std::string> seen;

  for (const std::string_view name : requested) {
    if (name == "all") {
      throw std::invalid_argument("adapter selection 'all' cannot be combined with explicit adapter names");
    }

    if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
      continue;
    }

    const auto it = std::find_if(
        descriptors.begin(),
        descriptors.end(),
        [name](const AdapterDescriptor& descriptor) {
          return descriptor.name == name;
        });
    if (it == descriptors.end()) {
      unknown.emplace_back(name);
      continue;
    }

    adapters.push_back(it->create());
    seen.emplace_back(name);
  }

  if (!unknown.empty()) {
    std::string message = "unknown adapter selection: ";
    for (std::size_t index = 0; index < unknown.size(); ++index) {
      if (index != 0U) {
        message += ", ";
      }
      message += unknown[index];
    }
    throw std::invalid_argument(message);
  }

  return adapters;
}

ResolveReport resolve_request(
    const ResolverEngine& engine,
    const ResolveRequest& request) {
  ResolveReport report = engine.resolve(request);

  if (request.reference_file.has_value()) {
    const ReferenceLocations references =
        load_reference_locations(*request.reference_file, request.kernel_name);
    for (const KernelResolution& resolution : report.resolutions) {
      if (references.availability == ReferenceAvailability::kNoVtuneSourceLocations) {
        ComparisonReport comparison;
        comparison.backend_name = resolution.backend_name;
        comparison.kernel_name = resolution.kernel_name;
        comparison.skip_reason = "VTune reference contains no source locations";
        report.comparisons.push_back(std::move(comparison));
      } else {
        report.comparisons.push_back(compare_locations(resolution, references.locations));
      }
    }
  }

  return report;
}

}  // namespace dwarf_parser_check