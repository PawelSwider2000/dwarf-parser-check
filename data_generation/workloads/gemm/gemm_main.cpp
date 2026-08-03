#include <sycl/sycl.hpp>
#include <sycl/ext/oneapi/backend/level_zero.hpp>

#include <level_zero/ze_api.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "kernel_debug_info.h"

namespace {

constexpr int kMatrixSize = 1024;
constexpr int kTotalLoops = 30;
constexpr float kAValue = 0.128f;
constexpr float kBValue = 0.256f;
constexpr float kMaxEps = 1.0e-4f;

struct Config {
  std::string jsonOutputPath = "simple_sycl_vtune_kernel_debug.json";
};

class PrimaryGEMMKernel;

void InitLevelZeroModuleDebugCollection(const Config &config) {
  std::cout << "[host] enabling Level Zero module debug collection\n";
  InitKernelTracer(std::filesystem::absolute(config.jsonOutputPath).parent_path());
}

void ShutdownLevelZeroModuleDebugCollection() {
  std::cout << "[host] disabling Level Zero module debug collection\n";
  DestroyKernelTracer();
}

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

void Usage(const char *name) {
  std::cerr << "Usage: " << name
            << " [json_output_path]\n"
            << "  json_output_path : Output JSON file path (default: simple_sycl_vtune_kernel_debug.json)\n";
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

void GEMM(const float *a, const float *b, float *c, unsigned size,
          sycl::id<2> id) {
  int row = id.get(0);
  int column = id.get(1);
  float sum = 0.0f;
  for (unsigned k = 0; k < size; ++k) {
    sum += a[row * size + k] * b[k * size + column];
  }
  c[row * size + column] = sum;
}

float CheckResults(const std::vector<float> &c, float expectedValue) {
  float eps = 0.0f;
  for (size_t i = 0; i < c.size(); ++i) {
    eps += std::fabs((c[i] - expectedValue) / expectedValue);
  }
  return eps / c.size();
}

template <typename KernelName>
bool RunGEMM(sycl::queue &queue, const std::vector<float> &a,
             const std::vector<float> &b, std::vector<float> &c,
             unsigned size, float expectedResult) {
  try {
    sycl::buffer<float, 1> aBuffer(a.data(), a.size());
    sycl::buffer<float, 1> bBuffer(b.data(), b.size());
    sycl::buffer<float, 1> cBuffer(c.data(), c.size());

    queue.submit([&](sycl::handler &handler) {
      auto aAccessor = aBuffer.get_access<sycl::access::mode::read>(handler);
      auto bAccessor = bBuffer.get_access<sycl::access::mode::read>(handler);
      auto cAccessor = cBuffer.get_access<sycl::access::mode::write>(handler);

      handler.parallel_for<KernelName>(sycl::range<2>(size, size),
                                       [=](sycl::id<2> id) {
        auto aPointer =
            aAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        auto bPointer =
            bAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        auto cPointer =
            cAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        GEMM(aPointer.get(), bPointer.get(), cPointer.get(), size, id);
      });
    });
    queue.wait_and_throw();
  } catch (const sycl::exception &e) {
    std::cerr << "SYCL Exception: " << e.what() << "\n";
    return false;
  }

  float eps = CheckResults(c, expectedResult);
  bool passed = eps < kMaxEps;
  std::cout << "GEMM results are " << (passed ? "CORRECT" : "INCORRECT")
            << " (accuracy: " << eps << ")\n";
  return passed;
}

template <typename KernelName>
bool RunGEMMWorkload(sycl::queue &queue, unsigned size,
                     const char *kernelName) {
  std::vector<float> a(static_cast<size_t>(size) * size, kAValue);
  std::vector<float> b(static_cast<size_t>(size) * size, kBValue);
  std::vector<float> c(static_cast<size_t>(size) * size, 0.0f);
  float expectedResult = kAValue * kBValue * size;

  std::cout << "[host] launching " << kernelName << " with matrix size "
            << size << "x" << size << "\n";
  return RunGEMM<KernelName>(queue, a, b, c, size, expectedResult);
}

} // namespace

int main(int argc, char *argv[]) {
  const Config config = ParseCommandLine(argc, argv);

  sycl::queue queue(sycl::gpu_selector_v, sycl::property::queue::in_order{});
  std::cout << "[host] device: "
            << queue.get_device().get_info<sycl::info::device::name>() << "\n\n";
  const std::optional<uint32_t> igaPlatform = GetIgaPlatform(queue.get_device());
  if (!igaPlatform.has_value()) {
    std::cerr << "[host] WARNING: unable to map the Level Zero device IP version to IGA\n";
  }

  InitLevelZeroModuleDebugCollection(config);
  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    const bool ok = RunGEMMWorkload<PrimaryGEMMKernel>(
        queue, kMatrixSize, "PrimaryGEMMKernel");
    if (!ok) {
      std::cerr << "[host] validation failed\n";
      ShutdownLevelZeroModuleDebugCollection();
      return EXIT_FAILURE;
    }
  }
  ShutdownLevelZeroModuleDebugCollection();

  if (!WriteCollectedKernelDebugDataJson(config, igaPlatform)) {
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
