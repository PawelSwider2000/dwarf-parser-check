#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli.h"
#include "core.h"
#include "kernel_debug_manifest.h"
#include "adapters/gimli/rust_gimli_adapter.h"

namespace dwarf_parser_check {
namespace {

std::filesystem::path sample_dwarf_path() {
  return std::filesystem::path(DPC_SOURCE_DIR) / "artifacts" /
      "_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE.dwarf";
}

bool ends_with(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

TEST(CliTest, ParsesAllIpsJsonRequest) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
  "--kernel-debug-json",
  "kernel_debug.json",
      "--all-ips",
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  ASSERT_TRUE(cli.has_value()) << errors.str();
  EXPECT_TRUE(cli->resolve_all_ips);
  EXPECT_EQ(cli->kernel_debug_json, "kernel_debug.json");
}

TEST(CliTest, AcceptsKernelDebugJsonAsRequestMetadata) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--kernel-debug-json",
      "kernel_debug.json",
      "--all-ips",
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
  EXPECT_TRUE(cli->resolve_all_ips);
}

TEST(CliTest, AllowsKernelDebugJsonWithoutIpSelection) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--kernel-debug-json",
      "kernel_debug.json",
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  ASSERT_TRUE(cli.has_value()) << errors.str();
  EXPECT_TRUE(cli->ips.empty());
  EXPECT_FALSE(cli->resolve_all_ips);
}

TEST(CliTest, ParsesCsvOutputPath) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--kernel-debug-json",
      "kernel_debug.json",
      "--output-csv",
      "result.csv",
  };
  std::vector<char*> argv;
  argv.reserve(args.size());
  for (std::string& arg : args) {
    argv.push_back(arg.data());
  }

  std::ostringstream errors;
  const auto cli = parse_cli(static_cast<int>(argv.size()), argv.data(), errors);

  ASSERT_TRUE(cli.has_value()) << errors.str();
  ASSERT_TRUE(cli->output_csv.has_value());
  EXPECT_EQ(*cli->output_csv, "result.csv");
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
      {"name":"first","mangled_name":"_ZFirst","demangled_name":"First","elf_dwarf_path":"/tmp/first.dwarf","runtime_kernel_address":"0x800000001000","kernel_binary_size":512},
      {"name":"second","mangled_name":"_ZSecond","demangled_name":"Second","elf_dwarf_path":"/tmp/second.dwarf"}
    ]})";
  }

  const std::vector<KernelDebugData> kernels = load_kernel_debug_manifest(manifest_path);
  std::filesystem::remove(manifest_path);

  ASSERT_EQ(kernels.size(), 2U);
  EXPECT_EQ(kernels[0].demangled_name, "First");
  EXPECT_EQ(kernels[0].runtime_kernel_address, 0x800000001000U);
  EXPECT_EQ(kernels[0].kernel_binary_size, 512U);
  EXPECT_EQ(kernels[1].demangled_name, "Second");

  const ResolveRequest first_request = make_resolve_request(kernels[0]);
  const ResolveRequest second_request = make_resolve_request(kernels[1]);
  EXPECT_EQ(first_request.dwarf_file, "/tmp/first.dwarf");
  EXPECT_EQ(second_request.mangled_kernel_name, "_ZSecond");
}

TEST(CoreTest, CreateAdaptersTrimsWhitespace) {
  auto adapters = create_adapters("  rust-gimli\t ");

  ASSERT_EQ(adapters.size(), 1U);
  ASSERT_NE(adapters[0], nullptr);
  EXPECT_EQ(adapters[0]->name(), "rust-gimli");
}

TEST(CliTest, PrintUsageListsCompiledAdapters) {
  std::ostringstream output;
  print_usage(output, "dwarf-parser-check");
  const std::string usage = output.str();

  EXPECT_NE(usage.find("Usage: dwarf-parser-check"), std::string::npos);
  EXPECT_NE(usage.find("compiled:"), std::string::npos);
  EXPECT_NE(usage.find("rust-gimli"), std::string::npos);
}

TEST(RustGimliAdapterTest, ResolveAllIpsMapsPrimaryBodyToUserSource) {
  auto adapter = make_rust_gimli_adapter();
  ResolveRequest request;
  request.dwarf_file = sample_dwarf_path();
  request.kernel_name = "(anonymous namespace)::PrimaryGEMMKernel";
  request.mangled_kernel_name = "_ZTSN12_GLOBAL__N_117PrimaryGEMMKernelE";
  request.runtime_kernel_address = 0x00008000fff80000ULL;
  request.kernel_binary_size = 81152;
  request.resolve_all_ips = true;

  ASSERT_TRUE(adapter->supports(request));
  const KernelResolution resolution = adapter->resolve_kernel(request);

  ASSERT_FALSE(resolution.locations.empty());
  EXPECT_EQ(resolution.backend_name, "rust-gimli");
  EXPECT_EQ(resolution.locations.front().location.ip, 0U);
  EXPECT_TRUE(ends_with(resolution.locations.front().location.file, "simple_sycl_vtune.cpp"));
  ASSERT_TRUE(resolution.locations.front().location.line.has_value());
}

}  // namespace
}  // namespace dwarf_parser_check