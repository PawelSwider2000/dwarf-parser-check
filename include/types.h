#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace dwarf_parser_check {

struct ResolveRequest {
  std::filesystem::path dwarf_file;
  std::string kernel_name;
  std::string mangled_kernel_name;
  std::uint64_t runtime_kernel_address = 0;
  std::size_t kernel_binary_size = 0;
  std::optional<std::filesystem::path> reference_file;
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
};

struct KernelResolution {
  std::string backend_name;
  std::string kernel_name;
  std::vector<SourceLocation> locations;
  std::vector<std::string> warnings;
};

}  // namespace dwarf_parser_check