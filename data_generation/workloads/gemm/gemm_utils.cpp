#include "gemm_utils.h"

#include <sycl/ext/oneapi/backend/level_zero.hpp>

#include <level_zero/ze_api.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

#include "kernel_debug_info.h"

namespace {

std::string EscapeJsonString(const std::string &value) {
  std::ostringstream escaped;
  for (char c : value) {
    switch (c) {
      case '\\':
        escaped << "\\\\";
        break;
      case '"':
        escaped << "\\\"";
        break;
      case '\b':
        escaped << "\\b";
        break;
      case '\f':
        escaped << "\\f";
        break;
      case '\n':
        escaped << "\\n";
        break;
      case '\r':
        escaped << "\\r";
        break;
      case '\t':
        escaped << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          escaped << "\\u00"
                  << "0123456789abcdef"[(static_cast<unsigned char>(c) >> 4) & 0x0f]
                  << "0123456789abcdef"[static_cast<unsigned char>(c) & 0x0f];
        } else {
          escaped << c;
        }
        break;
    }
  }
  return escaped.str();
}

std::string FormatHexAddress(uint64_t address) {
  std::ostringstream formatted;
  formatted << "0x" << std::hex << address;
  return formatted.str();
}

std::string BuildCollectedKernelDebugDataJson(
    const Config &config, const std::optional<uint32_t> &igaPlatform) {
  const size_t kernelCount = GetKernelDebugDataCount();
  std::ostringstream json;
  json << "{\n"
       << "  \"kernel_count\": " << kernelCount << ",\n"
       << "  \"json_output_path\": \""
       << EscapeJsonString(std::filesystem::absolute(config.jsonOutputPath).string())
       << "\",\n"
       << "  \"kernels\": [\n";

  for (size_t index = 0; index < kernelCount; ++index) {
    const KernelDebugData *dbg = GetKernelDebugDataByIndex(index);
    if (dbg == nullptr) {
      continue;
    }

    json << "    {\n"
         << "      \"name\": \"" << EscapeJsonString(dbg->name) << "\",\n"
         << "      \"mangled_name\": \"" << EscapeJsonString(dbg->mangled_name) << "\",\n"
         << "      \"demangled_name\": \"" << EscapeJsonString(dbg->demangled_name) << "\",\n"
         << "      \"elf_dwarf_path\": \"" << EscapeJsonString(dbg->elf_dwarf_path) << "\",\n"
         << "      \"module_handle_address\": \"" << FormatHexAddress(dbg->module_handle_address) << "\",\n"
         << "      \"kernel_handle_address\": \"" << FormatHexAddress(dbg->kernel_handle_address) << "\",\n"
         << "      \"runtime_kernel_address\": \"" << FormatHexAddress(dbg->runtime_kernel_address) << "\",\n"
         << "      \"module_debug_info_size\": " << dbg->module_debug_info_size << ",\n"
         << "      \"module_native_binary_size\": " << dbg->module_native_binary_size << ",\n"
         << "      \"kernel_binary_size\": ";
    if (dbg->kernel_binary_size_collected) {
      json << dbg->kernel_binary_size;
    } else {
      json << "null";
    }
    json << ",\n"
         << "      \"iga_platform\": ";
    if (igaPlatform.has_value()) {
      json << "\"" << FormatHexAddress(*igaPlatform) << "\"";
    } else {
      json << "null";
    }
    json << "\n"
         << "    }";
    if (index + 1 != kernelCount) {
      json << ",";
    }
    json << "\n";
  }

  json << "  ]\n"
       << "}\n";
  return json.str();
}

void Usage(const char *name) {
  std::cerr << "Usage: " << name
            << " [json_output_path]\n"
            << "  json_output_path : Output JSON file path (default: simple_sycl_vtune_kernel_debug.json)\n";
}

} // namespace

void InitLevelZeroModuleDebugCollection(const Config &config) {
  std::cout << "[host] enabling Level Zero module debug collection\n";
  InitKernelTracer(std::filesystem::absolute(config.jsonOutputPath).parent_path());
}

void ShutdownLevelZeroModuleDebugCollection() {
  std::cout << "[host] disabling Level Zero module debug collection\n";
  DestroyKernelTracer();
}

std::optional<uint32_t> GetIgaPlatform(const sycl::device &device) {
  ze_device_ip_version_ext_t ipVersion{
      ZE_STRUCTURE_TYPE_DEVICE_IP_VERSION_EXT, nullptr, 0};
  ze_device_properties_t properties{
      ZE_STRUCTURE_TYPE_DEVICE_PROPERTIES, &ipVersion};
  const ze_device_handle_t nativeDevice =
      sycl::get_native<sycl::backend::ext_oneapi_level_zero>(device);
  if (zeDeviceGetProperties(nativeDevice, &properties) != ZE_RESULT_SUCCESS ||
      ipVersion.ipVersion == 0) {
    return std::nullopt;
  }

  switch (ipVersion.ipVersion >> 24) {
    case 5:
      return 2U << 24;
    case 6:
      return 3U << 24;
    default:
      return std::nullopt;
  }
}

bool WriteCollectedKernelDebugDataJson(
    const Config &config, const std::optional<uint32_t> &igaPlatform) {
  std::ofstream output(config.jsonOutputPath, std::ios::binary | std::ios::trunc);
  if (!output) {
    std::cerr << "[host] failed to open JSON output file: "
              << config.jsonOutputPath << "\n";
    return false;
  }
  output << BuildCollectedKernelDebugDataJson(config, igaPlatform);
  std::cout << "[host] wrote kernel debug JSON to "
            << std::filesystem::absolute(config.jsonOutputPath).string()
            << "\n";
  return true;
}

Config ParseCommandLine(int argc, char *argv[]) {
  Config config;
  if (argc > 1) {
    config.jsonOutputPath = argv[1];
  }
  if (argc > 2) {
    Usage(argv[0]);
    std::exit(EXIT_FAILURE);
  }
  return config;
}