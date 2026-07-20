#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct DpcAddr2LineContext DpcAddr2LineContext;

typedef struct DpcAddr2LineLocation {
  uint64_t address;
  char* file;
  char* function_name;
  uint64_t line;
  uint64_t column;
  uint8_t has_line;
  uint8_t has_column;
} DpcAddr2LineLocation;

const char* dpc_addr2line_last_error(void);

DpcAddr2LineContext* dpc_addr2line_context_new(const char* path);
void dpc_addr2line_context_free(DpcAddr2LineContext* context);

int dpc_addr2line_resolve_address(
    DpcAddr2LineContext* context,
    uint64_t address,
    DpcAddr2LineLocation* location);

void dpc_addr2line_location_dispose(DpcAddr2LineLocation* location);

#ifdef __cplusplus
}
#endif
