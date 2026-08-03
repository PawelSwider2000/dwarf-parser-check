#pragma once

#include <sycl/sycl.hpp>

#include <cstdint>
#include <optional>
#include <string>

struct Config {
  std::string jsonOutputPath = "simple_sycl_vtune_kernel_debug.json";
};

void InitLevelZeroModuleDebugCollection(const Config &config);
void ShutdownLevelZeroModuleDebugCollection();
std::optional<uint32_t> GetIgaPlatform(const sycl::device &device);
bool WriteCollectedKernelDebugDataJson(
    const Config &config, const std::optional<uint32_t> &igaPlatform);
Config ParseCommandLine(int argc, char *argv[]);