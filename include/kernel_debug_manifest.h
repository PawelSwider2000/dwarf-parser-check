#pragma once

#include <filesystem>
#include <vector>

#include "kernel_debug_data.h"
#include "types.h"

namespace dwarf_parser_check {

[[nodiscard]] std::vector<KernelDebugData> load_kernel_debug_manifest(
    const std::filesystem::path& manifest_path);

[[nodiscard]] ResolveRequest make_resolve_request(const KernelDebugData& kernel);

}  // namespace dwarf_parser_check