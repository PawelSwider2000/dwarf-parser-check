#include "dummy_adapter.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dwarf_parser_check {
namespace {

class DummyAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "dummy";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty() && !request.kernel_name.empty() &&
        !request.mangled_kernel_name.empty();
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;
    resolution.warnings.push_back("Dummy adapter returns synthetic data only.");

    std::vector<std::uint64_t> ips = request.ips;
    if (ips.empty()) {
      ips = {0x10, 0x20, 0x30};
    }

    for (const std::uint64_t ip : ips) {
      SourceLocation location;
      location.location.kernel_name = request.kernel_name;
      location.location.ip = ip;
      location.location.file = "kernel_source.cpp";
      location.location.line = static_cast<std::uint64_t>((ip % 97U) + 1U);
      location.location.column = 1;
      location.backend_notes.push_back("Synthetic result generated for scaffolding.");

      InlineFrame frame;
      frame.function_name = request.kernel_name + "_helper";
      frame.file = location.location.file;
      frame.line = location.location.line;
      frame.column = 1;
      location.inline_chain.push_back(std::move(frame));

      resolution.locations.push_back(std::move(location));
    }

    std::sort(
        resolution.locations.begin(),
        resolution.locations.end(),
        [](const SourceLocation& left, const SourceLocation& right) {
          return left.location.ip < right.location.ip;
        });

    return resolution;
  }
};

}  // namespace

DwarfAdapterPtr make_dummy_adapter() {
  return std::make_unique<DummyAdapter>();
}

}  // namespace dwarf_parser_check