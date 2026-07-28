#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli.h"
#include "core.h"
#include "kernel_debug_manifest.h"
#if defined(DPC_HAVE_GDB_INTEL_ADAPTER)
#include "adapters/gdb/gdb_intel_adapter.h"
#endif
#include "adapters/gimli/rust_gimli_adapter.h"
#if defined(DPC_HAVE_IGA_ADAPTER)
#include "adapters/iga/iga_adapter.h"
#endif

namespace dwarf_parser_check {
namespace {

std::filesystem::path sample_dwarf_path() {
  return std::filesystem::path(DPC_SOURCE_DIR) / "artifacts" /
      "_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE.dwarf";
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

class FixedResolutionAdapter final : public DwarfAdapter {
 public:
  explicit FixedResolutionAdapter(std::string backend_name) : backend_name_(std::move(backend_name)) {}

  std::string name() const override { return backend_name_; }

  bool supports(const ResolveRequest&) const override { return true; }

  KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = backend_name_;
    resolution.kernel_name = request.kernel_name;
    SourceLocation location;
    location.location.kernel_name = request.kernel_name;
    location.location.ip = 0x40;
    location.location.file = "source.cpp";
    location.location.line = 10;
    resolution.locations.push_back(std::move(location));
    return resolution;
  }

 private:
  std::string backend_name_;
};

TEST(CliTest, ParsesKernelDebugJsonRequest) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
    "--kernel-debug-json",
    "kernel_debug.json",
    "--output-dir",
    "results",
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  ASSERT_TRUE(cli.has_value()) << errors.str();
  EXPECT_EQ(cli->kernel_debug_json, "kernel_debug.json");
}

TEST(CliTest, RejectsRemovedIpSelectionOptions) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--kernel-debug-json",
      "kernel_debug.json",
      "--ip",
      "0xffff8000fff86d00",
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  EXPECT_FALSE(cli.has_value());
  EXPECT_NE(errors.str().find("unknown argument: --ip"), std::string::npos);
}

TEST(CliTest, ParsesOutputDirectory) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--kernel-debug-json",
      "kernel_debug.json",
      "--output-dir",
      "results",
  };
  std::vector<char*> argv;
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  ASSERT_TRUE(cli.has_value()) << errors.str();
  EXPECT_EQ(cli->output_dir, "results");
}

TEST(CliTest, WritesResolvedLocationsAsCsv) {
  ResolveReport report;
  KernelResolution resolution;
  SourceLocation mapped;
  mapped.location.ip = 0x40;
  mapped.location.file = "source,with\"quote.cpp";
  mapped.location.line = 202;
  resolution.locations.push_back(mapped);

  SourceLocation unmapped;
  unmapped.location.ip = 0x50;
  resolution.locations.push_back(unmapped);
  report.resolutions.push_back(std::move(resolution));

  std::ostringstream output;
  write_report_csv(report, output);

  EXPECT_EQ(
      output.str(),
      "Kernel Offset,Source File,Source Line\n"
      "0x40,\"source,with\"\"quote.cpp\",202\n"
      "0x50,,\n");
}

TEST(CliTest, WritesComparisonReportAsJson) {
  ResolveReport report;
  KernelResolution resolution;
  resolution.backend_name = "test-adapter";
  resolution.kernel_name = "test-kernel";
  SourceLocation location;
  location.location.kernel_name = "test-kernel";
  location.location.ip = 0x40;
  location.location.file = "source\"file.cpp";
  location.location.line = 10;
  resolution.locations.push_back(location);
  report.resolutions.push_back(resolution);

  ComparisonReport comparison;
  comparison.backend_name = "test-adapter";
  comparison.kernel_name = "test-kernel";
  ComparisonItem item;
  item.resolved = location;
  item.status = ComparisonStatus::kMissingInReference;
  item.notes.push_back("missing reference");
  comparison.items.push_back(std::move(item));

  ComparisonItem match;
  match.resolved = location;
  match.reference = location.location;
  comparison.items.push_back(std::move(match));
  report.comparisons.push_back(std::move(comparison));

  std::ostringstream output;
  write_report_json(report, output);

  EXPECT_NE(output.str().find("\"schema_version\": 2"), std::string::npos);
  EXPECT_EQ(output.str().find("\"resolutions\""), std::string::npos);
  EXPECT_EQ(output.str().find("\"reference_candidates\""), std::string::npos);
  EXPECT_NE(output.str().find("\n      \"backend\": \"test-adapter\""), std::string::npos);
  EXPECT_NE(output.str().find("\"summary\": {"), std::string::npos);
  EXPECT_NE(output.str().find("source\\\"file.cpp"), std::string::npos);
  EXPECT_NE(output.str().find("\"missing_in_reference\""), std::string::npos);
  EXPECT_NE(output.str().find("\"matched_ips\""), std::string::npos);
  EXPECT_NE(output.str().find("\"0x40\""), std::string::npos);
  EXPECT_EQ(output.str().find("\"status\": \"match\""), std::string::npos);
}

TEST(CliTest, RejectsLegacyMetadataOptions) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--dwarf-file",
      sample_dwarf_path().string(),
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  EXPECT_FALSE(parse_cli(static_cast<int>(argv.size()), argv.data(), errors).has_value());
  EXPECT_NE(errors.str().find("unknown argument: --dwarf-file"), std::string::npos);
}

TEST(KernelDebugManifestTest, LoadsEveryKernelRecord) {
  const std::filesystem::path manifest_path =
      std::filesystem::temp_directory_path() / "dwarf_parser_check_manifest_test.json";
  {
    std::ofstream manifest(manifest_path);
    ASSERT_TRUE(manifest);
    manifest << R"({"kernels":[
      {"name":"first","mangled_name":"_ZFirst","demangled_name":"First","elf_dwarf_path":"/tmp/first.dwarf","runtime_kernel_address":"0x800000001000","kernel_binary_size":512,"iga_platform":"0x02000000"},
      {"name":"second","mangled_name":"_ZSecond","demangled_name":"Second","elf_dwarf_path":"/tmp/second.dwarf"}
    ]})";
  }

  const std::vector<KernelDebugData> kernels = load_kernel_debug_manifest(manifest_path);
  std::filesystem::remove(manifest_path);

  ASSERT_EQ(kernels.size(), 2U);
  EXPECT_EQ(kernels[0].demangled_name, "First");
  EXPECT_EQ(kernels[0].runtime_kernel_address, 0x800000001000U);
  EXPECT_EQ(kernels[0].kernel_binary_size, 512U);
  ASSERT_TRUE(kernels[0].iga_platform.has_value());
  EXPECT_EQ(*kernels[0].iga_platform, 0x02000000U);
  EXPECT_EQ(kernels[1].demangled_name, "Second");

  const ResolveRequest first_request = make_resolve_request(kernels[0]);
  const ResolveRequest second_request = make_resolve_request(kernels[1]);
  EXPECT_EQ(first_request.dwarf_file, "/tmp/first.dwarf");
  ASSERT_TRUE(first_request.iga_platform.has_value());
  EXPECT_EQ(*first_request.iga_platform, 0x02000000U);
  EXPECT_EQ(second_request.mangled_kernel_name, "_ZSecond");
}

TEST(CoreTest, CreateAdaptersTrimsWhitespace) {
  auto adapters = create_adapters("  rust-gimli\t ");

  ASSERT_EQ(adapters.size(), 1U);
  ASSERT_NE(adapters[0], nullptr);
  EXPECT_EQ(adapters[0]->name(), "rust-gimli");
}

TEST(CoreTest, ComparesEverySelectedAdapterAgainstVtuneJson) {
  const std::filesystem::path reference_path =
      std::filesystem::temp_directory_path() / "dwarf_parser_check_vtune_reference.json";
  {
    std::ofstream reference(reference_path);
    ASSERT_TRUE(reference);
    reference << R"({"0x40":[["generated.cpp",20],["source.cpp",10]]})";
  }

  std::vector<DwarfAdapterPtr> adapters;
  adapters.push_back(std::make_unique<FixedResolutionAdapter>("first"));
  adapters.push_back(std::make_unique<FixedResolutionAdapter>("second"));
  const ResolverEngine engine(make_registry(std::move(adapters)));

  ResolveRequest request;
  request.kernel_name = "test-kernel";
  request.reference_file = reference_path;
  const ResolveReport report = resolve_request(engine, request);
  std::filesystem::remove(reference_path);

  ASSERT_EQ(report.comparisons.size(), 2U);
  EXPECT_EQ(report.comparisons[0].backend_name, "first");
  EXPECT_EQ(report.comparisons[1].backend_name, "second");
  EXPECT_FALSE(report.comparisons[0].has_mismatches());
  EXPECT_FALSE(report.comparisons[1].has_mismatches());
  ASSERT_EQ(report.comparisons[0].items.size(), 1U);
  EXPECT_NE(
      report.comparisons[0].items[0].notes.end(),
      std::find(
          report.comparisons[0].items[0].notes.begin(),
          report.comparisons[0].items[0].notes.end(),
          "reference contains multiple source locations for this offset"));
}

TEST(ComparisonTest, LoadsEveryMappedVtuneCsvRow) {
  const std::filesystem::path reference_path =
      std::filesystem::temp_directory_path() / "dwarf_parser_check_vtune_reference.csv";
  {
    std::ofstream reference(reference_path);
    ASSERT_TRUE(reference);
    reference << "Address,Source File,Source Line,Assembly\n"
              << "0x40,,,Block 1:\n"
              << "0x40,source.cpp,10,\"mov r1, r2\"\n"
              << "0x50,other.cpp,20,nop\n";
  }

  const std::vector<Location> locations = load_reference_locations(reference_path, "test-kernel");
  std::filesystem::remove(reference_path);

  ASSERT_EQ(locations.size(), 2U);
  EXPECT_EQ(locations[0].ip, 0x40U);
  EXPECT_EQ(locations[0].file, "source.cpp");
  EXPECT_EQ(locations[1].ip, 0x50U);
  EXPECT_EQ(locations[1].line, 20U);
}

TEST(CliTest, PrintUsageListsCompiledAdapters) {
  std::ostringstream output;
  print_usage(output, "dwarf-parser-check");
  const std::string usage = output.str();

  EXPECT_NE(usage.find("Usage: dwarf-parser-check"), std::string::npos);
  EXPECT_NE(usage.find("compiled:"), std::string::npos);
  EXPECT_NE(usage.find("rust-gimli"), std::string::npos);
}

TEST(RustGimliAdapterTest, ResolvesWholeKernelToUserSource) {
  auto adapter = make_rust_gimli_adapter();
  ResolveRequest request;
  request.dwarf_file = sample_dwarf_path();
  request.kernel_name = "(anonymous namespace)::PrimaryGEMMKernel";
  request.mangled_kernel_name = "_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE";
  request.runtime_kernel_address = 0x00008000fff80000ULL;
  request.kernel_binary_size = 81152;

  ASSERT_TRUE(adapter->supports(request));
  const KernelResolution resolution = adapter->resolve_kernel(request);

  ASSERT_FALSE(resolution.locations.empty());
  EXPECT_EQ(resolution.backend_name, "rust-gimli");
  EXPECT_EQ(resolution.locations.front().location.ip, 0U);
  EXPECT_TRUE(ends_with(resolution.locations.front().location.file, "simple_sycl_vtune.cpp"));
  ASSERT_TRUE(resolution.locations.front().location.line.has_value());
}

#if defined(DPC_HAVE_GDB_INTEL_ADAPTER)
TEST(GdbIntelAdapterTest, RequiresAConfiguredExecutable) {
  const char* configured_path = std::getenv("DPC_GDB_ADDR2LINE");
  const std::optional<std::string> saved_path =
      configured_path == nullptr ? std::nullopt : std::optional<std::string>(configured_path);
  ASSERT_EQ(unsetenv("DPC_GDB_ADDR2LINE"), 0);

  auto adapter = make_gdb_intel_adapter();
  ResolveRequest request;
  request.dwarf_file = sample_dwarf_path();
  request.kernel_name = "test-kernel";

  const KernelResolution resolution = adapter->resolve_kernel(request);
  ASSERT_EQ(resolution.locations.size(), 0U);
  ASSERT_EQ(resolution.warnings.size(), 1U);

  if (saved_path.has_value()) {
    setenv("DPC_GDB_ADDR2LINE", saved_path->c_str(), 1);
  }
}
#endif

#if defined(DPC_HAVE_IGA_ADAPTER)
TEST(IgaAdapterTest, ResolvesDecodedInstructionsAndComparesWithVtune) {
  auto adapter = make_iga_adapter();
  ResolveRequest request;
  request.dwarf_file = sample_dwarf_path();
  request.kernel_name = "(anonymous namespace)::PrimaryGEMMKernel";
  request.mangled_kernel_name = "_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE";
  request.kernel_binary_size = 81152;
  request.iga_platform = 0x02000000;
  request.reference_file = std::filesystem::path(DPC_SOURCE_DIR) / "artifacts" / "source_locations.json";

  ASSERT_TRUE(adapter->supports(request));
  std::vector<DwarfAdapterPtr> adapters;
  adapters.push_back(std::move(adapter));
  const ResolverEngine engine(make_registry(std::move(adapters)));
  const ResolveReport report = resolve_request(engine, request);

  ASSERT_EQ(report.resolutions.size(), 1U);
  EXPECT_EQ(report.resolutions[0].backend_name, "iga");
  EXPECT_GE(report.resolutions[0].locations.size(), 5000U);
  ASSERT_FALSE(report.comparisons.empty());
  EXPECT_EQ(report.comparisons[0].backend_name, "iga");
  EXPECT_GT(
      std::count_if(
          report.comparisons[0].items.begin(),
          report.comparisons[0].items.end(),
          [](const ComparisonItem& item) {
            return item.status == ComparisonStatus::kMatch;
          }),
      0U);
  EXPECT_EQ(
      std::count_if(
          report.comparisons[0].items.begin(),
          report.comparisons[0].items.end(),
          [](const ComparisonItem& item) {
            return item.status == ComparisonStatus::kMissingInBackend;
          }),
      0U);

}
#endif

}  // namespace
}  // namespace dwarf_parser_check