#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "cli.h"
#include "core.h"
#include "adapters/gimli/rust_gimli_adapter.h"

namespace dwarf_parser_check {
namespace {

std::filesystem::path sample_dwarf_path() {
  return std::filesystem::path(DPC_SOURCE_DIR) / "dwarf_files" / "PrimaryGEMMKernel.dwarf";
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
  request.resolve_all_ips = true;

  ASSERT_TRUE(adapter->supports(request));
  const KernelResolution resolution = adapter->resolve_kernel(request);

  ASSERT_FALSE(resolution.locations.empty());
  EXPECT_EQ(resolution.backend_name, "rust-gimli");
  EXPECT_EQ(resolution.locations.front().location.ip, 0xffff8000fff86d00ULL);
  EXPECT_TRUE(ends_with(resolution.locations.front().location.file, "simple_sycl_vtune.cpp"));
  ASSERT_TRUE(resolution.locations.front().location.line.has_value());
  EXPECT_EQ(*resolution.locations.front().location.line, 211U);
}

}  // namespace
}  // namespace dwarf_parser_check