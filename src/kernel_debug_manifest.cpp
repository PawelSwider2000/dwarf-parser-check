#include "kernel_debug_manifest.h"

#include <fstream>
#include <iterator>
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
    kernels.push_back({
        find_string_field(record, "name"),
        find_string_field(record, "mangled_name"),
        find_string_field(record, "demangled_name"),
        find_string_field(record, "elf_dwarf_path"),
    });
  }

  if (kernels.empty()) {
    throw std::runtime_error("kernel debug JSON contains no kernel records");
  }
  return kernels;
}

ResolveRequest make_resolve_request(const KernelDebugData& kernel) {
  ResolveRequest request;
  request.dwarf_file = kernel.elf_dwarf_path;
  request.kernel_name = kernel.demangled_name;
  request.mangled_kernel_name = kernel.mangled_name;
  return request;
}

}  // namespace dwarf_parser_check
