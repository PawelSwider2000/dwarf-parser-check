#include "kernel_debug_info.h"

#include <level_zero/layers/zel_tracing_api.h>
#include <level_zero/ze_api.h>
#include <level_zero/zet_api.h>

#include <cxxabi.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using ZexKernelGetBaseAddress_t = ze_result_t (*)(ze_kernel_handle_t,
                                                  uint64_t *);

namespace {

std::mutex gDebugDataMutex;
std::vector<KernelDebugData> gKernelDebugData;
std::filesystem::path gArtifactDirectory;
zel_tracer_handle_t gTracer = nullptr;
ZexKernelGetBaseAddress_t gZexKernelGetBaseAddress = nullptr;

std::string DemangleKernelName(const std::string &mangledName) {
  int status = 0;
  char *demangled = abi::__cxa_demangle(mangledName.c_str(), nullptr, nullptr,
                                        &status);
  if (status != 0 || demangled == nullptr) {
    return mangledName;
  }

  std::string demangledName(demangled);
  std::free(demangled);

  constexpr const char kTypeInfoPrefix[] = "typeinfo name for ";
  if (demangledName.rfind(kTypeInfoPrefix, 0) == 0) {
    demangledName.erase(0, sizeof(kTypeInfoPrefix) - 1);
  }
  return demangledName;
}

void StoreKernelDebugData(KernelDebugData dbg) {
  std::lock_guard<std::mutex> lock(gDebugDataMutex);
  gKernelDebugData.push_back(std::move(dbg));
}

std::string WriteKernelDwarfFile(const std::string &kernelName,
                                 const std::vector<uint8_t> &debugInfo) {
  const std::filesystem::path dwarfFilePath =
      gArtifactDirectory / (kernelName + ".dwarf");
  std::ofstream dwarfFile(dwarfFilePath, std::ios::binary | std::ios::trunc);
  if (!dwarfFile) {
    std::cerr << "[tracer] WARNING: Failed to open " << dwarfFilePath
              << " for writing\n";
    return "";
  }

  dwarfFile.write(reinterpret_cast<const char *>(debugInfo.data()),
                  static_cast<std::streamsize>(debugInfo.size()));
  std::cout << "[tracer] Saved ELF/DWARF image to " << dwarfFilePath << "\n";
  return std::filesystem::absolute(dwarfFilePath).string();
}

void OnExitKernelCreate(ze_kernel_create_params_t *params,
                        ze_result_t result, void * /*globalData*/,
                        void ** /*instanceData*/) {
  if (result != ZE_RESULT_SUCCESS) {
    return;
  }

  ze_module_handle_t module = *(params->phModule);
  ze_kernel_handle_t kernel =
      params->pphKernel ? **(params->pphKernel) : nullptr;
  const ze_kernel_desc_t *desc = *(params->pdesc);
  const char *kernelName = desc != nullptr ? desc->pKernelName : nullptr;
  if (kernelName == nullptr) {
    return;
  }

  uint64_t runtimeKernelAddress = 0;
  if (kernel != nullptr && gZexKernelGetBaseAddress != nullptr) {
    ze_result_t status =
        gZexKernelGetBaseAddress(kernel, &runtimeKernelAddress);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[tracer] WARNING: zexKernelGetBaseAddress failed for "
                << kernelName << "\n";
      runtimeKernelAddress = 0;
    }
  }

  std::cout << "[tracer] Kernel created: " << kernelName << "\n";

  size_t moduleNativeBinarySize = 0;
  ze_result_t status =
      zeModuleGetNativeBinary(module, &moduleNativeBinarySize, nullptr);
  if (status != ZE_RESULT_SUCCESS) {
    std::cerr << "[tracer] WARNING: zeModuleGetNativeBinary size query failed for "
              << kernelName << "\n";
    moduleNativeBinarySize = 0;
  }

  bool kernelBinarySizeCollected = false;
  size_t kernelBinarySize = 0;
  if (kernel != nullptr) {
    status = zeKernelGetBinaryExp(kernel, &kernelBinarySize, nullptr);
    if (status != ZE_RESULT_SUCCESS) {
      std::cerr << "[tracer] WARNING: zeKernelGetBinaryExp size query failed for "
                << kernelName << "\n";
      kernelBinarySize = 0;
    } else {
      kernelBinarySizeCollected = true;
    }
  }

  size_t debugInfoSize = 0;
  status = zetModuleGetDebugInfo(module, ZET_MODULE_DEBUG_INFO_FORMAT_ELF_DWARF,
                                 &debugInfoSize, nullptr);
  if (status != ZE_RESULT_SUCCESS || debugInfoSize == 0) {
    std::cerr << "[tracer] WARNING: No debug info for " << kernelName
              << " (compile with -g or -gline-tables-only)\n";
    return;
  }

  std::vector<uint8_t> debugInfo(debugInfoSize);
  status = zetModuleGetDebugInfo(module, ZET_MODULE_DEBUG_INFO_FORMAT_ELF_DWARF,
                                 &debugInfoSize, debugInfo.data());
  if (status != ZE_RESULT_SUCCESS) {
    std::cerr << "[tracer] WARNING: zetModuleGetDebugInfo failed\n";
    return;
  }

  std::cout << "[tracer] Got " << debugInfoSize
            << " bytes of ELF/DWARF debug info\n";
  const std::string elfDwarfPath = WriteKernelDwarfFile(kernelName, debugInfo);

  KernelDebugData debugData;
  debugData.name = kernelName;
  debugData.mangled_name = kernelName;
  debugData.demangled_name = DemangleKernelName(kernelName);
  debugData.elf_dwarf_path = elfDwarfPath;
  debugData.module_handle_address = reinterpret_cast<uint64_t>(module);
  debugData.kernel_handle_address = reinterpret_cast<uint64_t>(kernel);
  debugData.runtime_kernel_address = runtimeKernelAddress;
  debugData.module_debug_info_size = debugInfoSize;
  debugData.module_native_binary_size = moduleNativeBinarySize;
  debugData.kernel_binary_size_collected = kernelBinarySizeCollected;
  debugData.kernel_binary_size = kernelBinarySize;
  StoreKernelDebugData(std::move(debugData));
}

} // namespace

void InitKernelTracer(const std::filesystem::path &artifactDirectory) {
  gArtifactDirectory = artifactDirectory;
  ze_driver_handle_t driver = nullptr;
  uint32_t driverCount = 1;
  if (zeDriverGet(&driverCount, &driver) == ZE_RESULT_SUCCESS &&
      driver != nullptr) {
    zeDriverGetExtensionFunctionAddress(
        driver, "zexKernelGetBaseAddress",
        reinterpret_cast<void **>(&gZexKernelGetBaseAddress));
  }

  zel_tracer_desc_t tracerDesc = {ZEL_STRUCTURE_TYPE_TRACER_EXP_DESC, nullptr,
                                  nullptr};
  ze_result_t status = zelTracerCreate(&tracerDesc, &gTracer);
  if (status != ZE_RESULT_SUCCESS) {
    std::cerr << "[tracer] WARNING: zelTracerCreate failed 0x" << std::hex
              << status << std::dec << ", kernel debug info unavailable\n";
    return;
  }

  zet_core_callbacks_t epilogueCallbacks{};
  epilogueCallbacks.Kernel.pfnCreateCb = OnExitKernelCreate;
  zelTracerSetEpilogues(gTracer, &epilogueCallbacks);
  zelTracerSetEnabled(gTracer, true);

  std::cout << "[tracer] Kernel creation tracer enabled\n";
}

void DestroyKernelTracer() {
  if (gTracer != nullptr) {
    zelTracerSetEnabled(gTracer, false);
    zelTracerDestroy(gTracer);
    gTracer = nullptr;
  }
}

size_t GetKernelDebugDataCount() {
  std::lock_guard<std::mutex> lock(gDebugDataMutex);
  return gKernelDebugData.size();
}

const KernelDebugData *GetKernelDebugDataByIndex(size_t index) {
  std::lock_guard<std::mutex> lock(gDebugDataMutex);
  if (index >= gKernelDebugData.size()) {
    return nullptr;
  }
  return &gKernelDebugData[index];
}
