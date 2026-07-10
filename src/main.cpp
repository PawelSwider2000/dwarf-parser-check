#include <exception>
#include <iostream>

#include "cli.h"
#include "core.h"

int main(int argc, char** argv) {
  using namespace dwarf_parser_check;

  const auto cli = parse_cli(argc, argv, std::cerr);
  if (!cli.has_value()) {
    print_usage(std::cerr, argc > 0 ? argv[0] : "dwarf-parser-check");
    return 1;
  }

  if (cli->show_help) {
    print_usage(std::cout, argc > 0 ? argv[0] : "dwarf-parser-check");
    return 0;
  }

  try {
    ResolverEngine engine(make_default_registry());
    const ResolveReport report = resolve_request(engine, cli->request, cli->resolve_options);
    print_report(report, std::cout);
    return report.empty() ? 1 : 0;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}