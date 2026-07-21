#include "adapters/gimli/rust_gimli_adapter.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include "adapters/gimli/rust_addr2line_ffi.h"

namespace dwarf_parser_check {
namespace {

std::string consume_c_string(char*& value) {
  if (value == nullptr) {
    return {};
  }

  std::string copy(value);
  value = nullptr;
  return copy;
}

std::uint64_t canonicalize_gpu_address(std::uint64_t address) {
  constexpr std::uint64_t kAddressMask = (1ULL << 48U) - 1U;
  constexpr std::uint64_t kSignBit = 1ULL << 47U;
  constexpr std::uint64_t kSignExtension = ~kAddressMask;
  address &= kAddressMask;
  return (address & kSignBit) != 0U ? address | kSignExtension : address;
}

std::uint64_t relative_kernel_address(
    std::uint64_t address,
    std::uint64_t runtime_kernel_address) {
  if (runtime_kernel_address == 0U) {
    return address;
  }

  const std::uint64_t kernel_begin =
      canonicalize_gpu_address(runtime_kernel_address);
  return address >= kernel_begin ? address - kernel_begin : address;
}

class RustGimliAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "rust-gimli";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty() && std::filesystem::exists(request.dwarf_file);
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;

    DpcAddr2LineContext* context = dpc_addr2line_context_new(request.dwarf_file.c_str());
    if (context == nullptr) {
      resolution.warnings.push_back(last_error("failed to create Rust addr2line context"));
      return resolution;
    }

    std::vector<std::uint64_t> ips = request.ips;
    if (request.resolve_all_ips && ips.empty()) {
      if (request.mangled_kernel_name.empty() ||
          request.runtime_kernel_address == 0U ||
          request.kernel_binary_size == 0U) {
        resolution.warnings.push_back(
            "kernel debug JSON is missing mangled_name, runtime_kernel_address, or kernel_binary_size.");
        dpc_addr2line_context_free(context);
        return resolution;
      }

      DpcAddr2LineAddresses addresses{};
      const int status = dpc_addr2line_enumerate_kernel_ips(
          context,
          request.mangled_kernel_name.c_str(),
          canonicalize_gpu_address(request.runtime_kernel_address),
          request.kernel_binary_size,
          &addresses);
      if (status < 0) {
        resolution.warnings.push_back(last_error("Rust addr2line kernel IP enumeration failed"));
      } else if (status == 0) {
        resolution.warnings.push_back(last_error("Rust addr2line found no kernel IPs"));
      } else {
        ips.assign(addresses.values, addresses.values + addresses.len);
      }
      dpc_addr2line_addresses_dispose(&addresses);
    }

    std::sort(ips.begin(), ips.end());
    ips.erase(std::unique(ips.begin(), ips.end()), ips.end());

    for (const std::uint64_t ip : ips) {
      DpcAddr2LineLocation native_location{};
      const int status = dpc_addr2line_resolve_address(context, ip, &native_location);

      if (status == 1) {
        SourceLocation location;
        location.location.kernel_name = request.kernel_name;
        location.location.ip =
          relative_kernel_address(ip, request.runtime_kernel_address);
        location.location.file = consume_c_string(native_location.file);
        location.location.line = native_location.has_line != 0U
            ? std::optional<std::uint64_t>(native_location.line)
            : std::nullopt;
        location.location.column = native_location.has_column != 0U
            ? std::optional<std::uint64_t>(native_location.column)
            : std::nullopt;

        const std::string function_name = consume_c_string(native_location.function_name);
        if (!function_name.empty() && function_name != request.kernel_name) {
          location.backend_notes.push_back("resolved function: " + function_name);
        }

        dpc_addr2line_location_dispose(&native_location);
        resolution.locations.push_back(std::move(location));
        continue;
      }

      dpc_addr2line_location_dispose(&native_location);
      if (status < 0) {
        resolution.warnings.push_back(last_error("Rust addr2line lookup failed for IP 0x" + to_hex(ip)));
      }
    }

    dpc_addr2line_context_free(context);
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

  static std::string to_hex(std::uint64_t value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 0; index < text.size(); ++index) {
      const std::size_t shift = (text.size() - 1U - index) * 4U;
      text[index] = kDigits[(value >> shift) & 0xFU];
    }

    const std::size_t first_digit = text.find_first_not_of('0');
    return first_digit == std::string::npos ? "0" : text.substr(first_digit);
  }
};

}  // namespace

DwarfAdapterPtr make_rust_gimli_adapter() {
  return std::make_unique<RustGimliAdapter>();
}

}  // namespace dwarf_parser_check
