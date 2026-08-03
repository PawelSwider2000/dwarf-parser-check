#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace dwarf_parser_check {

struct InputIp {
  std::uint64_t address = 0;
  std::size_t line_number = 0;
};

struct NormalizedIp {
  std::uint64_t input_address = 0;
  std::optional<std::uint64_t> kernel_offset;
  std::size_t line_number = 0;
};

enum class IpResolutionStatus {
  kResolved,
  kOutsideKernel,
  kNoSourceLocation,
  kResolverError,
};

struct IpResolutionResult {
  NormalizedIp input;
  IpResolutionStatus status = IpResolutionStatus::kNoSourceLocation;
  std::optional<SourceLocation> location;
  std::string reason;
};

[[nodiscard]] std::uint64_t canonicalize_intel_gpu_address(std::uint64_t address) noexcept;

[[nodiscard]] std::vector<InputIp> load_ip_list(const std::filesystem::path& path);

[[nodiscard]] std::vector<NormalizedIp> normalize_ip_list(
    const std::vector<InputIp>& inputs,
    std::uint64_t kernel_base,
  std::uint64_t kernel_size,
  std::uint64_t section_file_offset = 0);

[[nodiscard]] std::vector<IpResolutionResult> resolve_normalized_ips(
    const std::vector<NormalizedIp>& inputs,
    const KernelResolution* resolution,
    std::string_view resolver_error = {});

void write_resolved_ip_csv(
    const std::vector<IpResolutionResult>& results,
    std::ostream& output);

void write_unresolved_ip_csv(
    const std::vector<IpResolutionResult>& results,
    std::ostream& output);

}  // namespace dwarf_parser_check