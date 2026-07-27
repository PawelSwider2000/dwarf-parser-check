#pragma once

#include <cstddef>
#include <cstdint>

extern "C" {

using iga_context_t = void*;

enum iga_status_t : int {
  IGA_SUCCESS = 0,
};

struct iga_context_options_t {
  std::size_t cb;
  std::uint32_t gen;
};

struct iga_disassemble_options_t {
  std::uint32_t cb;
  std::uint32_t formatting_opts;
  std::uint32_t reserved0;
  std::uint32_t reserved1;
  std::uint32_t decoder_opts;
  std::uint32_t base_pc_offset;
};

const char* iga_status_to_string(iga_status_t status);
iga_status_t iga_context_create(const iga_context_options_t* options, iga_context_t* context);
iga_status_t iga_context_release(iga_context_t context);
iga_status_t iga_context_disassemble(
    iga_context_t context,
    const iga_disassemble_options_t* options,
    const void* input,
    std::uint32_t input_size,
    const char* (*format_label)(std::int32_t, void*),
    void* format_label_context,
    char** kernel_text);
  iga_status_t iga_context_disassemble_instruction(
    iga_context_t context,
    const iga_disassemble_options_t* options,
    const void* input,
    const char* (*format_label)(std::int32_t, void*),
    void* format_label_context,
    char** kernel_text);

}  // extern "C"

constexpr std::uint32_t kIgaFormattingPrintPc = 0x00000008U;