#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "cli.h"
#include "core.h"

namespace {

std::optional<std::string> json_string_value(std::string_view json,
                                             std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const std::size_t key_position = json.find(needle);
  if (key_position == std::string_view::npos) {
    return std::nullopt;
  }

  const std::size_t colon_position = json.find(':', key_position + needle.size());
  if (colon_position == std::string_view::npos) {
    return std::nullopt;
  }

  std::size_t position = json.find('"', colon_position + 1);
  if (position == std::string_view::npos) {
    return std::nullopt;
  }
  ++position;

  std::string value;
  while (position < json.size()) {
    const char character = json[position++];
    if (character == '"') {
      return value;
    }
    if (character != '\\' || position == json.size()) {
      value += character;
      continue;
    }

    const char escaped = json[position++];
    switch (escaped) {
      case '"': value += '"'; break;
      case '\\': value += '\\'; break;
      case '/': value += '/'; break;
      case 'b': value += '\b'; break;
      case 'f': value += '\f'; break;
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

[[nodiscard]] dwarf_parser_check::ResolveRequest load_kernel_debug_request(
  const std::filesystem::path& json_path) {
  std::ifstream input(json_path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to open kernel debug JSON: " + json_path.string());
  }

  const std::string json((std::istreambuf_iterator<char>(input)),
                         std::istreambuf_iterator<char>());
  const auto dwarf_path = json_string_value(json, "elf_dwarf_path");
  const auto demangled_name = json_string_value(json, "demangled_name");
  const auto mangled_name = json_string_value(json, "mangled_name");
  if (!dwarf_path.has_value() || !demangled_name.has_value() || !mangled_name.has_value()) {
    throw std::runtime_error("kernel debug JSON must contain elf_dwarf_path, demangled_name, and mangled_name");
  }

  dwarf_parser_check::ResolveRequest request;
  request.dwarf_file = *dwarf_path;
  request.kernel_name = *demangled_name;
  request.mangled_kernel_name = *mangled_name;
  return request;
}

}  // namespace

int main(int argc, char** argv) {
  using namespace dwarf_parser_check;

  auto cli = parse_cli(argc, argv, std::cerr);
  if (!cli.has_value()) {
    print_usage(std::cerr, argc > 0 ? argv[0] : "dwarf-parser-check");
    return 1;
  }

  if (cli->show_help) {
    print_usage(std::cout, argc > 0 ? argv[0] : "dwarf-parser-check");
    return 0;
  }

  try {
    ResolveRequest request = load_kernel_debug_request(cli->kernel_debug_json);
    request.ips = cli->ips;
    request.resolve_all_ips = cli->resolve_all_ips;
    request.reference_file = cli->reference_file;
    if (request.ips.empty() && !request.resolve_all_ips) {
      request.resolve_all_ips = true;
    }
    ResolverEngine engine(make_registry(create_adapters(cli->adapter_selection)));
    const ResolveReport report = resolve_request(engine, request);
    print_report(report, std::cout);
    return report.empty() ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}