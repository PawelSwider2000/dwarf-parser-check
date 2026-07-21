#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "cli.h"
#include "core.h"
#include "kernel_debug_manifest.h"

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
    ResolverEngine engine(make_registry(create_adapters(cli->adapter_selection)));
    const std::vector<KernelDebugData> kernels =
        load_kernel_debug_manifest(cli->kernel_debug_json);
    if (kernels.empty()) {
      std::cerr << "error: kernel debug JSON contains no kernels\n";
      return 1;
    }

    std::ofstream csv_output;
    if (cli->output_csv.has_value()) {
      csv_output.open(*cli->output_csv);
      if (!csv_output) {
        throw std::runtime_error("unable to open CSV output file: " + cli->output_csv->string());
      }
    }

    std::ofstream json_output;
    if (cli->output_json.has_value()) {
      json_output.open(*cli->output_json);
      if (!json_output) {
        throw std::runtime_error("unable to open JSON output file: " + cli->output_json->string());
      }
    }

    bool resolved_any_kernel = false;
    ResolveReport csv_report;
    ResolveReport output_report;
    for (const KernelDebugData& kernel : kernels) {
      ResolveRequest request = make_resolve_request(kernel);
      request.ips = cli->ips;
      request.resolve_all_ips = cli->resolve_all_ips;
      request.reference_file = cli->reference_file;
      if (request.ips.empty() && !request.resolve_all_ips) {
        request.resolve_all_ips = true;
      }

      const ResolveReport report = resolve_request(engine, request);
      print_report(report, std::cout);
      if (csv_output) {
        csv_report.resolutions.insert(
            csv_report.resolutions.end(), report.resolutions.begin(), report.resolutions.end());
      }
      if (json_output) {
        output_report.resolutions.insert(
            output_report.resolutions.end(), report.resolutions.begin(), report.resolutions.end());
        output_report.comparisons.insert(
            output_report.comparisons.end(), report.comparisons.begin(), report.comparisons.end());
        output_report.diagnostics.insert(
            output_report.diagnostics.end(), report.diagnostics.begin(), report.diagnostics.end());
      }
      resolved_any_kernel = resolved_any_kernel || !report.empty();
    }
    if (csv_output) {
      write_report_csv(csv_report, csv_output);
    }
    if (json_output) {
      write_report_json(output_report, json_output);
    }
    return resolved_any_kernel ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}