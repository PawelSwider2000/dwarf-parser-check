#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "cli.h"
#include "core.h"
#include "ip_resolution.h"
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

std::string join_messages(const std::vector<std::string>& messages) {
  std::string result;
  for (const std::string& message : messages) {
    if (!result.empty()) {
      result += "; ";
    }
    result += message;
  }
  return result;
}

std::vector<std::uint64_t> addr2line_ips_from_normalized(
    const std::vector<dwarf_parser_check::NormalizedIp>& normalized,
    std::uint64_t kernel_base) {
  std::vector<std::uint64_t> addresses;
  addresses.reserve(normalized.size());
  const std::uint64_t canonical_kernel_base =
      dwarf_parser_check::canonicalize_intel_gpu_address(kernel_base);
  for (const dwarf_parser_check::NormalizedIp& input : normalized) {
    if (input.kernel_offset.has_value()) {
      addresses.push_back(canonical_kernel_base + *input.kernel_offset);
    }
  }
  return addresses;
}

std::vector<std::uint64_t> load_addr2line_ips(
    const std::filesystem::path& path,
    std::uint64_t kernel_base,
    std::uint64_t kernel_size,
    std::uint64_t section_file_offset) {
  const std::vector<dwarf_parser_check::InputIp> inputs =
      dwarf_parser_check::load_ip_list(path);
  return addr2line_ips_from_normalized(
      dwarf_parser_check::normalize_ip_list(
          inputs, kernel_base, kernel_size, section_file_offset),
      kernel_base);
}

int resolve_ip_list(const dwarf_parser_check::CliOptions& cli) {
  using namespace dwarf_parser_check;

  if (*cli.kernel_size > std::numeric_limits<std::size_t>::max()) {
    throw std::runtime_error("--kernel-size exceeds the platform size limit");
  }

  const std::vector<InputIp> inputs = load_ip_list(*cli.ip_list);
  const std::vector<NormalizedIp> normalized = normalize_ip_list(
      inputs, *cli.kernel_base, *cli.kernel_size);

  std::vector<DwarfAdapterPtr> adapters = create_adapters(cli.resolver_selection);
  if (adapters.size() != 1U) {
    throw std::runtime_error("--resolver must select exactly one compiled resolver backend");
  }
  std::vector<DwarfAdapterPtr> single_adapter;
  single_adapter.push_back(std::move(adapters.front()));
  const ResolverEngine engine(make_registry(std::move(single_adapter)));

  ResolveRequest request;
  request.dwarf_file = cli.dwarf_file;
  request.kernel_name = cli.kernel_name;
  request.mangled_kernel_name = cli.kernel_symbol;
  request.runtime_kernel_address = canonicalize_intel_gpu_address(*cli.kernel_base);
  request.kernel_binary_size = static_cast<std::size_t>(*cli.kernel_size);
  request.addr2line_ips = addr2line_ips_from_normalized(normalized, *cli.kernel_base);
  const ResolveReport report = engine.resolve(request);

  const KernelResolution* resolution = nullptr;
  std::string resolver_error;
  if (report.resolutions.size() == 1U) {
    resolution = &report.resolutions.front();
    if (resolution->locations.empty() && !resolution->warnings.empty()) {
      resolver_error = join_messages(resolution->warnings);
    }
  } else {
    resolver_error = report.diagnostics.empty()
        ? "selected resolver did not return a resolution"
        : join_messages(report.diagnostics);
  }

  const std::vector<IpResolutionResult> results =
      resolve_normalized_ips(normalized, resolution, resolver_error);
  for (const std::filesystem::path* output_path :
       {&cli.resolved_output, &cli.unresolved_output}) {
    if (!output_path->parent_path().empty()) {
      std::filesystem::create_directories(output_path->parent_path());
    }
  }

  std::ofstream resolved_output(cli.resolved_output);
  if (!resolved_output) {
    throw std::runtime_error("unable to open resolved output file: " + cli.resolved_output.string());
  }
  write_resolved_ip_csv(results, resolved_output);

  std::ofstream unresolved_output(cli.unresolved_output);
  if (!unresolved_output) {
    throw std::runtime_error(
        "unable to open unresolved output file: " + cli.unresolved_output.string());
  }
  write_unresolved_ip_csv(results, unresolved_output);

  std::size_t resolved_count = 0;
  for (const IpResolutionResult& result : results) {
    if (result.status == IpResolutionStatus::kResolved) {
      ++resolved_count;
    }
  }
  std::cout << "resolved " << resolved_count << " of " << results.size() << " input IPs\n";
  return resolved_count == 0U ? 1 : 0;
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
    if (cli->ip_list.has_value()) {
      return resolve_ip_list(*cli);
    }

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
        request.runtime_kernel_address = canonicalize_intel_gpu_address(request.runtime_kernel_address);
        // Per-kernel reference from manifest takes priority over --reference.
        if (const auto* entry = find_manifest_entry(kernel.name)) {
          request.reference_file = entry->reference_csv;
          if (entry->ip_list.empty()) {
            throw std::runtime_error("VTune manifest is missing an IP list for kernel " + kernel.name);
          }
          request.addr2line_ips = load_addr2line_ips(
              entry->ip_list, kernel.runtime_kernel_address, kernel.kernel_binary_size,
              entry->section_file_offset);
        } else {
          request.reference_file = cli->reference_file;
          throw std::runtime_error("VTune manifest has no IP list for kernel " + kernel.name);
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
        const bool skipped_reference = std::any_of(
          adapter_report.comparisons.begin(), adapter_report.comparisons.end(),
          [](const ComparisonReport& comparison) { return comparison.is_skipped(); });
        resolved_any_kernel = resolved_any_kernel || !adapter_report.empty() || skipped_reference;
    }
    return resolved_any_kernel ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 1;
  }
}