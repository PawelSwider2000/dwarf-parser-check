#include "ip_resolution.h"

#include <charconv>
#include <fstream>
#include <stdexcept>
#include <string_view>

namespace dwarf_parser_check {
namespace {

std::string_view trim(std::string_view value) {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r')) {
    value.remove_suffix(1U);
  }
  return value;
}

std::optional<std::uint64_t> parse_hex_address(std::string_view value) {
  if (value.size() >= 2U && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
    value.remove_prefix(2U);
  }
  if (value.empty()) {
    return std::nullopt;
  }

  std::uint64_t address = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), address, 16);
  if (error != std::errc() || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return address;
}

}  // namespace

std::uint64_t canonicalize_intel_gpu_address(std::uint64_t address) noexcept {
  constexpr std::uint64_t kAddressMask = (UINT64_C(1) << 48) - 1;
  constexpr std::uint64_t kSignBit = UINT64_C(1) << 47;
  return (address & kAddressMask) | ((address & kSignBit) != 0U ? ~kAddressMask : 0U);
}

std::vector<InputIp> load_ip_list(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("unable to open IP list: " + path.string());
  }

  std::vector<InputIp> addresses;
  std::string line;
  std::size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    const std::size_t comment = line.find('#');
    const std::string_view value = trim(std::string_view(line).substr(0, comment));
    if (value.empty()) {
      continue;
    }
    const std::optional<std::uint64_t> address = parse_hex_address(value);
    if (!address.has_value()) {
      throw std::runtime_error(
          "invalid hexadecimal IP at " + path.string() + ":" + std::to_string(line_number));
    }
    addresses.push_back({*address, line_number});
  }
  return addresses;
}

std::vector<NormalizedIp> normalize_ip_list(
    const std::vector<InputIp>& inputs,
    std::uint64_t kernel_base,
  std::uint64_t kernel_size,
  std::uint64_t section_file_offset) {
  const std::uint64_t normalized_base = canonicalize_intel_gpu_address(kernel_base);
  std::vector<NormalizedIp> normalized;
  normalized.reserve(inputs.size());
  for (const InputIp& input : inputs) {
    NormalizedIp value;
    value.input_address = input.address;
    value.line_number = input.line_number;
    const std::uint64_t normalized_address = canonicalize_intel_gpu_address(input.address);
    if (section_file_offset != 0U && input.address >= section_file_offset &&
        input.address - section_file_offset < kernel_size) {
      value.kernel_offset = input.address - section_file_offset;
    } else if (input.address < kernel_size) {
      value.kernel_offset = input.address;
    } else if (normalized_address >= normalized_base) {
      const std::uint64_t offset = normalized_address - normalized_base;
      if (offset < kernel_size) {
        value.kernel_offset = offset;
      }
    }
    normalized.push_back(std::move(value));
  }
  return normalized;
}

}  // namespace dwarf_parser_check