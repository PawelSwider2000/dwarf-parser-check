#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct KernelDebugData {
  std::string name;
  std::string mangled_name;
  std::string demangled_name;
  std::string elf_dwarf_path;
  uint64_t module_handle_address = 0;
  uint64_t kernel_handle_address = 0;
  uint64_t runtime_kernel_address = 0;
  size_t module_debug_info_size = 0;
  size_t module_native_binary_size = 0;
};

void InitKernelTracer();
void DestroyKernelTracer();
size_t GetKernelDebugDataCount();
const KernelDebugData *GetKernelDebugDataByIndex(size_t index);
