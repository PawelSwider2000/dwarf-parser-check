#include <exception>
#include <iostream>

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

    bool resolved_any_kernel = false;
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
      resolved_any_kernel = resolved_any_kernel || !report.empty();
    }
    return resolved_any_kernel ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}