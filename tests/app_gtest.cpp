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

TEST(CliTest, ParsesAllIpsRequest) {
  std::vector<std::string> args = {
      "dwarf-parser-check",
      "--dwarf-file",
      sample_dwarf_path().string(),
      "--kernel",
      "PrimaryGEMM",
      "--mangled-kernel",
      "_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE",
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
  EXPECT_TRUE(cli->request.resolve_all_ips);
  EXPECT_EQ(cli->request.kernel_name, "PrimaryGEMM");
  EXPECT_EQ(cli->request.mangled_kernel_name, "_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE");
  EXPECT_EQ(cli->request.dwarf_file, sample_dwarf_path());
}

TEST(CoreTest, CreateAdaptersTrimsWhitespace) {
  auto adapters = create_adapters("  dummy\t ");

  ASSERT_EQ(adapters.size(), 1U);
  ASSERT_NE(adapters[0], nullptr);
  EXPECT_EQ(adapters[0]->name(), "dummy");
}

TEST(CliTest, PrintUsageListsCompiledAdapters) {
  std::ostringstream output;
  print_usage(output, "dwarf-parser-check");
  const std::string usage = output.str();

  EXPECT_NE(usage.find("Usage: dwarf-parser-check"), std::string::npos);
  EXPECT_NE(usage.find("compiled:"), std::string::npos);
  EXPECT_NE(usage.find("dummy"), std::string::npos);
}

TEST(RustGimliAdapterTest, ResolveAllIpsMapsPrimaryBodyToUserSource) {
  auto adapter = make_rust_gimli_adapter();
  ResolveRequest request;
  request.dwarf_file = sample_dwarf_path();
  request.kernel_name = "PrimaryGEMM";
  request.mangled_kernel_name = "_Z4GEMMPKfS0_PfjN4sycl3_V12idILi2EEE";
  request.resolve_all_ips = true;

  ASSERT_TRUE(adapter->supports(request));
  const KernelResolution resolution = adapter->resolve_kernel(request);

  ASSERT_FALSE(resolution.locations.empty());
  EXPECT_EQ(resolution.backend_name, "rust-gimli");
  EXPECT_EQ(resolution.locations.front().location.ip, 0x8000ffd50060ULL);
  EXPECT_TRUE(ends_with(resolution.locations.front().location.file, "main.cc"));
  ASSERT_TRUE(resolution.locations.front().location.line.has_value());
  EXPECT_EQ(*resolution.locations.front().location.line, 63U);
}

}  // namespace
}  // namespace dwarf_parser_check