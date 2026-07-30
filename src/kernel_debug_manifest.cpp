#include "kernel_debug_manifest.h"

#include <fstream>
#include <iterator>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace dwarf_parser_check {
namespace {

std::string unescape_json_string(std::string_view value) {
  std::string unescaped;
  unescaped.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] != '\\') {
      unescaped += value[index];
      continue;
    }
    if (++index == value.size()) {
      throw std::runtime_error("invalid escape in kernel debug JSON");
    }

    switch (value[index]) {
      case '"': unescaped += '"'; break;
      case '\\': unescaped += '\\'; break;
      case '/': unescaped += '/'; break;
      case 'b': unescaped += '\b'; break;
      case 'f': unescaped += '\f'; break;
      case 'n': unescaped += '\n'; break;
      case 'r': unescaped += '\r'; break;
      case 't': unescaped += '\t'; break;
      default: throw std::runtime_error("invalid escape in kernel debug JSON");
    }
  }
  return unescaped;
}

std::string find_string_field(const std::string& record, const char* field) {
  const std::regex pattern(std::string("\\\"") + field +
                           "\\\"\\s*:\\s*\\\"((?:\\\\.|[^\\\"])*)\\\"");
  std::smatch match;
  if (!std::regex_search(record, match, pattern)) {
    throw std::runtime_error(std::string("kernel record is missing ") + field);
  }
  return unescape_json_string(match[1].str());
}

std::optional<std::uint64_t> find_uint64_field(
    const std::string& record,
    const char* field) {
  const std::regex pattern(std::string("\\\"") + field +
                           "\\\"\\s*:\\s*(?:\\\")?(0[xX][0-9a-fA-F]+|[0-9]+)(?:\\\")?");
  std::smatch match;
  if (!std::regex_search(record, match, pattern)) {
    return std::nullopt;
  }

  try {
    return std::stoull(match[1].str(), nullptr, 0);
  } catch (const std::exception&) {
    throw std::runtime_error(std::string("invalid unsigned integer for ") + field);
  }
}

}  // namespace

std::vector<KernelDebugData> load_kernel_debug_manifest(
    const std::filesystem::path& manifest_path) {
  std::ifstream input(manifest_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open kernel debug JSON: " + manifest_path.string());
  }

  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::regex record_pattern("\\{([^{}]*)\\}");
  std::vector<KernelDebugData> kernels;
  for (std::sregex_iterator it(json.begin(), json.end(), record_pattern), end;
       it != end; ++it) {
    const std::string record = (*it)[1].str();
    if (record.find("\"mangled_name\"") == std::string::npos) {
      continue;
    }
    KernelDebugData kernel;
    kernel.name = find_string_field(record, "name");
    kernel.mangled_name = find_string_field(record, "mangled_name");
    kernel.demangled_name = find_string_field(record, "demangled_name");
    kernel.elf_dwarf_path = find_string_field(record, "elf_dwarf_path");
    kernel.runtime_kernel_address =
      find_uint64_field(record, "runtime_kernel_address").value_or(0);
    if (const auto binary_size = find_uint64_field(record, "kernel_binary_size")) {
      kernel.kernel_binary_size_collected = true;
      kernel.kernel_binary_size = static_cast<std::size_t>(*binary_size);
    }
    if (const auto iga_platform = find_uint64_field(record, "iga_platform")) {
      if (*iga_platform > UINT32_MAX) {
        throw std::runtime_error("iga_platform did not fit in an IGA platform value");
      }
      kernel.iga_platform = static_cast<std::uint32_t>(*iga_platform);
    }
    kernels.push_back(std::move(kernel));
  }

  if (kernels.empty()) {
    throw std::runtime_error("kernel debug JSON contains no kernel records");
  }
  return kernels;
}

std::vector<VtuneManifestEntry> load_vtune_manifest(
    const std::filesystem::path& manifest_path) {
  std::ifstream input(manifest_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open vtune manifest: " + manifest_path.string());
  }
  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const std::regex record_pattern(R"(\{([^{}]*)\})");
  std::vector<VtuneManifestEntry> entries;
  for (std::sregex_iterator it(json.begin(), json.end(), record_pattern), end;
       it != end; ++it) {
    const std::string record = (*it)[1].str();
    if (record.find("\"name\"") == std::string::npos) {
      continue;
    }
    VtuneManifestEntry entry;
    entry.kernel_name = find_string_field(record, "name");
    entry.source_locations = [&]() -> std::string {
      try { return find_string_field(record, "source_locations"); }
      catch (...) { return {}; }
    }();
    entry.reference_csv = [&]() -> std::string {
      try { return find_string_field(record, "reference_csv"); }
      catch (...) { return {}; }
    }();
    entry.section_file_offset =
        find_uint64_field(record, "section_file_offset").value_or(0);
    entries.push_back(std::move(entry));
  }
  return entries;
}

ResolveRequest make_resolve_request(const KernelDebugData& kernel) {
  ResolveRequest request;
  request.dwarf_file = kernel.elf_dwarf_path;
  request.kernel_name = kernel.demangled_name;
  request.mangled_kernel_name = kernel.mangled_name;
  request.runtime_kernel_address = kernel.runtime_kernel_address;
  request.kernel_binary_size = kernel.kernel_binary_size;
  request.iga_platform = kernel.iga_platform;
  return request;
}

}  // namespace dwarf_parser_check
