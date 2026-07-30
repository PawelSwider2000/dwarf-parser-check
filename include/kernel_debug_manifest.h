#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "kernel_debug_data.h"
#include "types.h"

namespace dwarf_parser_check {

[[nodiscard]] std::vector<KernelDebugData> load_kernel_debug_manifest(
    const std::filesystem::path& manifest_path);

[[nodiscard]] ResolveRequest make_resolve_request(const KernelDebugData& kernel);

struct VtuneManifestEntry {
  std::string kernel_name;  // matches KernelDebugData::name (mangled)
  std::filesystem::path reference_csv;
  std::uint64_t section_file_offset = 0;  // zebin file offset of the .text section
};

/// Load the vtune_manifest.json written by correlate_vtune_report.py.
/// Returns one entry per kernel section found in the zebin.
[[nodiscard]] std::vector<VtuneManifestEntry> load_vtune_manifest(
    const std::filesystem::path& manifest_path);

}  // namespace dwarf_parser_check