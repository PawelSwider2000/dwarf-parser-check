#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dwarf_parser_check {

struct KernelDebugData {
  std::string name;
  std::string mangled_name;
  std::string demangled_name;
  std::string elf_dwarf_path;
  std::uint64_t module_handle_address = 0;
  std::uint64_t kernel_handle_address = 0;
  std::uint64_t runtime_kernel_address = 0;
  std::size_t module_debug_info_size = 0;
  std::size_t module_native_binary_size = 0;
  bool kernel_binary_size_collected = false;
  std::size_t kernel_binary_size = 0;
};

}  // namespace dwarf_parser_check