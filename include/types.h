#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dwarf_parser_check {

enum class PathKind {
  kUnknown,
  kUser,
  kSystem,
};

struct ResolveRequest {
  std::filesystem::path dwarf_file;
  std::string kernel_name;
  std::vector<std::uint64_t> ips;
  bool resolve_all_ips = false;
  std::optional<std::filesystem::path> reference_file;
  std::optional<std::filesystem::path> project_root;
};

struct InlineFrame {
  std::string function_name;
  std::string file;
  std::optional<std::uint64_t> line;
  std::optional<std::uint64_t> column;
};

struct Location {
  std::string kernel_name;
  std::uint64_t ip = 0;
  std::string file;
  std::optional<std::uint64_t> line;
  std::optional<std::uint64_t> column;
};

struct SourceLocation {
  Location location;
  std::vector<InlineFrame> inline_chain;
  std::vector<std::string> backend_notes;
  PathKind path_kind = PathKind::kUnknown;

  [[nodiscard]] bool is_user_code_candidate() const noexcept {
    return path_kind == PathKind::kUser;
  }
};

struct KernelResolution {
  std::string backend_name;
  std::string kernel_name;
  std::vector<SourceLocation> locations;
  std::vector<std::string> warnings;
};

}  // namespace dwarf_parser_check