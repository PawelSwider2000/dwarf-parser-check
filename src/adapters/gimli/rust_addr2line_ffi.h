#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DpcAddr2LineKernelLocation {
  uint64_t offset;
  char* file;
  char* function_name;
  uint64_t line;
  uint64_t column;
  uint8_t has_line;
  uint8_t has_column;
} DpcAddr2LineKernelLocation;

typedef struct DpcAddr2LineKernelLocations {
  DpcAddr2LineKernelLocation* values;
  size_t len;
} DpcAddr2LineKernelLocations;

const char* dpc_addr2line_last_error(void);

int dpc_addr2line_resolve_kernel(
    const char* dwarf_path,
    const char* mangled_kernel_name,
    uint64_t runtime_kernel_address,
    size_t kernel_binary_size,
    DpcAddr2LineKernelLocations* locations);

void dpc_addr2line_kernel_locations_dispose(DpcAddr2LineKernelLocations* locations);

#ifdef __cplusplus
}
#endif
