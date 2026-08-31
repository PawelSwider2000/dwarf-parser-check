#include "adapters/pti/pti_elf_parser_adapter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "elf_parser.h"
#include "elf_parser.hpp"

namespace dwarf_parser_check {
namespace {

std::optional<std::uint32_t> matching_kernel_index(
    const elf_parser::ElfParser& parser,
    const ResolveRequest& request) {
  const std::vector<std::string> kernel_names = parser.GetKernelNames();
  const auto it = std::find(kernel_names.begin(), kernel_names.end(), request.mangled_kernel_name);
  if (it == kernel_names.end()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(std::distance(kernel_names.begin(), it));
}

std::string source_path(const SourceMapping& mapping) {
  const std::string directory = mapping.file_path == nullptr ? "" : mapping.file_path;
  const std::string filename = mapping.file_name == nullptr ? "" : mapping.file_name;
  if (directory.empty()) {
    return filename;
  }
  if (filename.empty()) {
    return directory;
  }
  return directory.ends_with('/') ? directory + filename : directory + "/" + filename;
}

class PtiElfParserAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "pti-elf-parser";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty() && !request.mangled_kernel_name.empty();
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;

    if (request.runtime_kernel_address == 0U) {
      resolution.warnings.push_back("kernel debug JSON is missing runtime_kernel_address.");
      return resolution;
    }
    if (request.addr2line_ips.empty()) {
      resolution.warnings.push_back("no addr2line IPs were supplied for this kernel.");
      return resolution;
    }

    std::ifstream input(request.dwarf_file, std::ios::binary);
    if (!input) {
      resolution.warnings.push_back("failed to open ELF/DWARF file: " + request.dwarf_file.string());
      return resolution;
    }
    std::vector<std::uint8_t> elf_data(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (elf_data.empty() || elf_data.size() > UINT32_MAX) {
      resolution.warnings.push_back("ELF/DWARF file is empty or exceeds the PTI parser size limit.");
      return resolution;
    }

    auto parser = std::make_unique<elf_parser::ElfParser>(
      elf_data.data(), static_cast<std::uint32_t>(elf_data.size()));
    if (!parser->IsValid()) {
      // PTI rejects a valid string table whose final byte is exactly at EOF.
      if (elf_data.size() == UINT32_MAX) {
        resolution.warnings.push_back("PTI GPU ELF parser rejected the ELF/DWARF file.");
        return resolution;
      }
      elf_data.push_back(0U);
      parser = std::make_unique<elf_parser::ElfParser>(
          elf_data.data(), static_cast<std::uint32_t>(elf_data.size()));
      if (!parser->IsValid()) {
        resolution.warnings.push_back("PTI GPU ELF parser rejected the ELF/DWARF file.");
        return resolution;
      }
    }
    const std::optional<std::uint32_t> kernel_index = matching_kernel_index(*parser, request);
    if (!kernel_index.has_value()) {
      resolution.warnings.push_back("PTI GPU ELF parser did not find the requested kernel symbol.");
      return resolution;
    }
    const std::uint64_t elf_kernel_address = parser->GetKernelAddress(*kernel_index);
    if (elf_kernel_address == UINT64_MAX) {
      resolution.warnings.push_back("PTI GPU ELF parser did not find the requested kernel text section.");
      return resolution;
    }

    std::vector<SourceMapping> mappings = parser->GetSourceMappingMatrix(*kernel_index);
    if (mappings.empty()) {
      resolution.warnings.push_back("PTI GPU ELF parser found no DWARF source mappings for the kernel.");
      return resolution;
    }
    std::sort(mappings.begin(), mappings.end(), [](const SourceMapping& left, const SourceMapping& right) {
      return left.address < right.address;
    });

    for (const std::uint64_t runtime_ip : request.addr2line_ips) {
      if (runtime_ip < request.runtime_kernel_address) {
        continue;
      }
      const std::uint64_t offset = runtime_ip - request.runtime_kernel_address;
      if (offset > UINT64_MAX - elf_kernel_address) {
        continue;
      }
      const std::uint64_t dwarf_address = elf_kernel_address + offset;
      const auto next = std::upper_bound(
          mappings.begin(), mappings.end(), dwarf_address,
          [](std::uint64_t address, const SourceMapping& mapping) {
            return address < mapping.address;
          });
      if (next == mappings.begin()) {
        continue;
      }
      const SourceMapping& mapping = *std::prev(next);
      if (mapping.is_end_sequence) {
        continue;
      }
      const std::string file = source_path(mapping);
      if (file.empty()) {
        continue;
      }

      SourceLocation location;
      location.location.kernel_name = request.kernel_name;
      location.location.ip = offset;
      location.location.file = file;
      location.location.line = mapping.line;
      if (mapping.column != 0U) {
        location.location.column = mapping.column;
      }
      resolution.locations.push_back(std::move(location));
    }
    if (resolution.locations.empty()) {
      resolution.warnings.push_back("PTI GPU ELF parser found no source locations for the supplied addr2line IPs.");
    }
    return resolution;
  }
};

}  // namespace

DwarfAdapterPtr make_pti_elf_parser_adapter() {
  return std::make_unique<PtiElfParserAdapter>();
}

}  // namespace dwarf_parser_check