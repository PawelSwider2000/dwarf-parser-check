#pragma once

#include <filesystem>

#include "../include/kernel_debug_data.h"

using dwarf_parser_check::KernelDebugData;

void InitKernelTracer(const std::filesystem::path &artifactDirectory);
void DestroyKernelTracer();
size_t GetKernelDebugDataCount();
const KernelDebugData *GetKernelDebugDataByIndex(size_t index);
