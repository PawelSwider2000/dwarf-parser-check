#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "core.h"
#include "kernel_debug_manifest.h"

namespace {

std::string adapter_file_name(std::string_view adapter_name, std::string_view suffix) {
  std::string result = "adapter_";
  for (const unsigned char character : adapter_name) {
    if ((character >= 'a' && character <= 'z') ||
        (character >= 'A' && character <= 'Z') ||
        (character >= '0' && character <= '9') || character == '-' || character == '_') {
      result += static_cast<char>(character);
    } else {
      result += '_';
    }
  }
  return result + std::string(suffix);
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
    const std::vector<KernelDebugData> kernels =
        load_kernel_debug_manifest(cli->kernel_debug_json);
    if (kernels.empty()) {
      std::cerr << "error: kernel debug JSON contains no kernels\n";
      return 1;
    }

    // Build a name→reference_csv lookup from the vtune manifest (if provided).
    std::vector<VtuneManifestEntry> vtune_manifest;
    if (cli->vtune_manifest.has_value()) {
      vtune_manifest = load_vtune_manifest(*cli->vtune_manifest);
    }
    auto find_manifest_entry = [&](const std::string& name)
        -> const VtuneManifestEntry* {
      for (const auto& entry : vtune_manifest) {
        if (entry.kernel_name == name) {
          return &entry;
        }
      }
      return nullptr;
    };

    std::filesystem::create_directories(cli->output_dir);
    std::vector<DwarfAdapterPtr> adapters = create_adapters(cli->adapter_selection);
    if (adapters.empty()) {
      throw std::runtime_error("adapter selection did not produce any compiled adapters");
    }

    bool resolved_any_kernel = false;
    for (DwarfAdapterPtr& adapter : adapters) {
      const std::string adapter_name = adapter->name();
      std::vector<DwarfAdapterPtr> single_adapter;
      single_adapter.push_back(std::move(adapter));
      ResolverEngine engine(make_registry(std::move(single_adapter)));

      ResolveReport adapter_report;
      for (const KernelDebugData& kernel : kernels) {
        ResolveRequest request = make_resolve_request(kernel);
        // Per-kernel reference from manifest takes priority over --reference.
        if (const auto* entry = find_manifest_entry(kernel.name)) {
          request.reference_file = entry->reference_csv;
        } else {
          request.reference_file = cli->reference_file;
        }

        const ResolveReport report = resolve_request(engine, request);
        print_report(report, std::cout);
        adapter_report.resolutions.insert(
            adapter_report.resolutions.end(), report.resolutions.begin(), report.resolutions.end());
        adapter_report.comparisons.insert(
            adapter_report.comparisons.end(), report.comparisons.begin(), report.comparisons.end());
        adapter_report.diagnostics.insert(
            adapter_report.diagnostics.end(), report.diagnostics.begin(), report.diagnostics.end());
      }

      const std::filesystem::path csv_path = cli->output_dir /
          adapter_file_name(adapter_name, "_result_dwarf_parser.csv");
      std::ofstream csv_output(csv_path);
      if (!csv_output) {
        throw std::runtime_error("unable to open CSV output file: " + csv_path.string());
      }
      write_report_csv(adapter_report, csv_output);

      const std::filesystem::path json_path = cli->output_dir /
          adapter_file_name(adapter_name, "_vtune_comparison.json");
      std::ofstream json_output(json_path);
      if (!json_output) {
        throw std::runtime_error("unable to open JSON output file: " + json_path.string());
      }
      write_report_json(adapter_report, json_output);
      resolved_any_kernel = resolved_any_kernel || !adapter_report.empty();
    }
    return resolved_any_kernel ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}