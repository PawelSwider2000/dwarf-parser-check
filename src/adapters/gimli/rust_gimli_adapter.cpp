#include "adapters/gimli/rust_gimli_adapter.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "adapters/gimli/rust_addr2line_ffi.h"

namespace dwarf_parser_check {
namespace {

struct KernelLocationsCleanup {
  DpcAddr2LineKernelLocations* locations;

  ~KernelLocationsCleanup() {
    dpc_addr2line_kernel_locations_dispose(locations);
  }
};

class RustGimliAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "rust-gimli";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty();
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;

    if (request.mangled_kernel_name.empty() ||
        request.runtime_kernel_address == 0U ||
        request.kernel_binary_size == 0U) {
      resolution.warnings.push_back(
          "kernel debug JSON is missing mangled_name, runtime_kernel_address, or kernel_binary_size.");
      return resolution;
    }

    DpcAddr2LineKernelLocations locations{};
    const KernelLocationsCleanup locations_cleanup{&locations};
    const int status = dpc_addr2line_resolve_kernel(
        request.dwarf_file.c_str(),
        request.mangled_kernel_name.c_str(),
        request.runtime_kernel_address,
        request.kernel_binary_size,
        &locations);
    if (status < 0) {
      resolution.warnings.push_back(last_error("Rust addr2line whole-kernel resolution failed"));
    } else if (status == 0) {
      resolution.warnings.push_back(last_error("Rust addr2line found no source locations"));
    } else {
      resolution.locations.reserve(locations.len);
      for (std::size_t index = 0; index < locations.len; ++index) {
        const DpcAddr2LineKernelLocation& native_location = locations.values[index];
        SourceLocation location;
        location.location.kernel_name = request.kernel_name;
        location.location.ip = native_location.offset;
        location.location.file = native_location.file == nullptr ? "" : native_location.file;
        location.location.line = native_location.has_line != 0U
            ? std::optional<std::uint64_t>(native_location.line)
            : std::nullopt;
        location.location.column = native_location.has_column != 0U
            ? std::optional<std::uint64_t>(native_location.column)
            : std::nullopt;

        const std::string function_name =
          native_location.function_name == nullptr ? "" : native_location.function_name;
        if (!function_name.empty() && function_name != request.kernel_name) {
          location.backend_notes.push_back("resolved function: " + function_name);
        }

        resolution.locations.push_back(std::move(location));
      }
    }

    return resolution;
  }

 private:
  static std::string last_error(const std::string& fallback_prefix) {
    const char* error = dpc_addr2line_last_error();
    if (error == nullptr || std::strlen(error) == 0) {
      return fallback_prefix;
    }

    return fallback_prefix + ": " + error;
  }

};

}  // namespace

DwarfAdapterPtr make_rust_gimli_adapter() {
  return std::make_unique<RustGimliAdapter>();
}

}  // namespace dwarf_parser_check
