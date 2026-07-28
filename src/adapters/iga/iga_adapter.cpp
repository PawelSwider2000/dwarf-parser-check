#include "adapters/iga/iga_adapter.h"

#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

#include "adapters/iga/iga_api.h"

namespace dwarf_parser_check {
namespace {

struct ElfCleanup {
  Elf* elf = nullptr;
  int file_descriptor = -1;

  ~ElfCleanup() {
    if (elf != nullptr) {
      elf_end(elf);
    }
    if (file_descriptor >= 0) {
      close(file_descriptor);
    }
  }
};

struct DwflCleanup {
  Dwfl* dwfl = nullptr;

  ~DwflCleanup() {
    if (dwfl != nullptr) {
      dwfl_end(dwfl);
    }
  }
};

struct IgaContextCleanup {
  iga_context_t context = nullptr;

  ~IgaContextCleanup() {
    if (context != nullptr) {
      iga_context_release(context);
    }
  }
};

struct KernelText {
  std::vector<std::uint8_t> bytes;
  std::uint64_t dwarf_address = 0;
};

struct DwarfLineLocation {
  std::uint64_t address = 0;
  std::string file;
  std::uint64_t line = 0;
  std::optional<std::uint64_t> column;
};

std::string iga_status_message(iga_status_t status) {
  const char* message = iga_status_to_string(status);
  return message == nullptr ? "unknown IGA failure" : message;
}

std::optional<std::uint32_t> request_iga_platform(
    const ResolveRequest& request,
    std::string& error) {
  if (!request.iga_platform.has_value()) {
    error = "kernel debug JSON is missing iga_platform.";
    return std::nullopt;
  }
  return request.iga_platform;
}

std::optional<KernelText> load_kernel_text(
    const std::filesystem::path& path,
    const std::string& symbol_name,
  std::size_t kernel_binary_size,
    std::string& error) {
  if (elf_version(EV_CURRENT) == EV_NONE) {
    error = "libelf initialization failed";
    return std::nullopt;
  }

  ElfCleanup cleanup;
  cleanup.file_descriptor = open(path.c_str(), O_RDONLY);
  if (cleanup.file_descriptor < 0) {
    error = "failed to open ELF/DWARF file: " + path.string();
    return std::nullopt;
  }
  cleanup.elf = elf_begin(cleanup.file_descriptor, ELF_C_READ, nullptr);
  if (cleanup.elf == nullptr) {
    error = "failed to read ELF/DWARF file: " + std::string(elf_errmsg(-1));
    return std::nullopt;
  }

  Elf_Scn* symbol_section = nullptr;
  GElf_Shdr symbol_section_header{};
  Elf_Scn* section = nullptr;
  while ((section = elf_nextscn(cleanup.elf, section)) != nullptr) {
    GElf_Shdr header{};
    if (gelf_getshdr(section, &header) == nullptr) {
      continue;
    }
    if (header.sh_type == SHT_SYMTAB || header.sh_type == SHT_DYNSYM) {
      symbol_section = section;
      symbol_section_header = header;
      break;
    }
  }
  if (symbol_section == nullptr) {
    error = "ELF/DWARF file has no symbol table";
    return std::nullopt;
  }

  Elf_Data* symbol_data = elf_getdata(symbol_section, nullptr);
  if (symbol_data == nullptr || symbol_section_header.sh_entsize == 0U) {
    error = "failed to read ELF symbol table";
    return std::nullopt;
  }
  const std::size_t symbol_count =
      static_cast<std::size_t>(symbol_section_header.sh_size / symbol_section_header.sh_entsize);
  GElf_Sym kernel_symbol{};
  bool found = false;
  for (std::size_t index = 0; index < symbol_count; ++index) {
    GElf_Sym symbol{};
    if (gelf_getsym(symbol_data, static_cast<int>(index), &symbol) == nullptr ||
        GELF_ST_TYPE(symbol.st_info) != STT_FUNC || symbol.st_size == 0U) {
      continue;
    }
    const char* name = elf_strptr(cleanup.elf, symbol_section_header.sh_link, symbol.st_name);
    if (name != nullptr && symbol_name == name) {
      kernel_symbol = symbol;
      found = true;
      break;
    }
  }
  if (!found) {
    error = "no nonempty text symbol matched kernel " + symbol_name;
    return std::nullopt;
  }

  Elf_Scn* text_section = elf_getscn(cleanup.elf, kernel_symbol.st_shndx);
  GElf_Shdr text_header{};
  Elf_Data* text_data = text_section == nullptr ? nullptr : elf_getdata(text_section, nullptr);
  if (text_section == nullptr || gelf_getshdr(text_section, &text_header) == nullptr ||
      text_data == nullptr || text_data->d_buf == nullptr || kernel_symbol.st_value < text_header.sh_addr) {
    error = "failed to read kernel text section";
    return std::nullopt;
  }

  const std::uint64_t section_offset = kernel_symbol.st_value - text_header.sh_addr;
  if (section_offset > text_data->d_size) {
    error = "kernel text symbol extends beyond its ELF section";
    return std::nullopt;
  }
  const std::size_t available_bytes = text_data->d_size - section_offset;
  const std::size_t bytes_to_decode =
      kernel_binary_size == 0U ? available_bytes : kernel_binary_size;
  if (bytes_to_decode > available_bytes) {
    error = "runtime kernel binary size extends beyond its ELF text section";
    return std::nullopt;
  }

  KernelText text;
  const auto* first_byte = static_cast<const std::uint8_t*>(text_data->d_buf) + section_offset;
  text.bytes.assign(first_byte, first_byte + bytes_to_decode);
  text.dwarf_address = kernel_symbol.st_value;
  return text;
}

std::optional<std::vector<DwarfLineLocation>> load_dwarf_line_locations(
    Dwfl_Module* module,
    std::string& error) {
  Dwarf_Addr bias = 0;
  Dwarf* dwarf = dwfl_module_getdwarf(module, &bias);
  if (dwarf == nullptr) {
    error = "libdw failed to load DWARF line information: " + std::string(dwfl_errmsg(-1));
    return std::nullopt;
  }

  std::vector<DwarfLineLocation> locations;
  Dwarf_CU* current_cu = nullptr;
  Dwarf_CU* next_cu = nullptr;
  Dwarf_Half version = 0;
  std::uint8_t unit_type = 0;
  Dwarf_Die cu_die{};
  while (dwarf_get_units(
             dwarf,
             current_cu,
             &next_cu,
             &version,
             &unit_type,
             &cu_die,
             nullptr) == 0) {
    current_cu = next_cu;
    Dwarf_Lines* lines = nullptr;
    std::size_t line_count = 0;
    if (dwarf_getsrclines(&cu_die, &lines, &line_count) != 0) {
      continue;
    }

    for (std::size_t index = 0; index < line_count; ++index) {
      Dwarf_Line* dwarf_line = dwarf_onesrcline(lines, index);
      Dwarf_Addr address = 0;
      int line_number = 0;
      if (dwarf_line == nullptr ||
          dwarf_lineaddr(dwarf_line, &address) != 0 ||
          dwarf_lineno(dwarf_line, &line_number) != 0 ||
          line_number <= 0) {
        continue;
      }
      const char* file = dwarf_linesrc(dwarf_line, nullptr, nullptr);
      if (file == nullptr || *file == '\0') {
        continue;
      }

      int column_number = 0;
      DwarfLineLocation location;
      location.address = address + bias;
      location.file = file;
      location.line = static_cast<std::uint64_t>(line_number);
      if (dwarf_linecol(dwarf_line, &column_number) == 0 && column_number > 0) {
        location.column = static_cast<std::uint64_t>(column_number);
      }
      locations.push_back(std::move(location));
    }
  }

  std::sort(
      locations.begin(),
      locations.end(),
      [](const DwarfLineLocation& left, const DwarfLineLocation& right) {
        return left.address < right.address;
      });
  if (locations.empty()) {
    error = "libdw found no usable DWARF line-table entries.";
    return std::nullopt;
  }
  return locations;
}

std::optional<SourceLocation> resolve_source_location(
    const std::vector<DwarfLineLocation>& dwarf_locations,
    const ResolveRequest& request,
    std::uint64_t offset,
    std::uint64_t dwarf_address) {
  const auto next_location = std::upper_bound(
      dwarf_locations.begin(),
      dwarf_locations.end(),
      dwarf_address,
      [](std::uint64_t address, const DwarfLineLocation& location) {
        return address < location.address;
      });
  if (next_location == dwarf_locations.begin()) {
    return std::nullopt;
  }
  const DwarfLineLocation& dwarf_location = *std::prev(next_location);

  SourceLocation location;
  location.location.kernel_name = request.kernel_name;
  location.location.ip = offset;
  location.location.file = dwarf_location.file;
  location.location.line = dwarf_location.line;
  location.location.column = dwarf_location.column;
  return location;
}

class IgaAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "iga";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty() && !request.mangled_kernel_name.empty();
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;

    std::string load_error;
    const std::optional<KernelText> kernel_text =
      load_kernel_text(
        request.dwarf_file,
        request.mangled_kernel_name,
        request.kernel_binary_size,
        load_error);
    if (!kernel_text.has_value()) {
      resolution.warnings.push_back(load_error);
      return resolution;
    }
    if (kernel_text->bytes.size() > UINT32_MAX) {
      resolution.warnings.push_back("ELF text symbol is too large for the IGA C API.");
      return resolution;
    }

    std::string platform_error;
    const std::optional<std::uint32_t> platform = request_iga_platform(request, platform_error);
    if (!platform.has_value()) {
      resolution.warnings.push_back(platform_error);
      return resolution;
    }

    const iga_context_options_t context_options{sizeof(iga_context_options_t), *platform};
    IgaContextCleanup context;
    const iga_status_t create_status = iga_context_create(&context_options, &context.context);
    if (create_status != IGA_SUCCESS) {
      resolution.warnings.push_back("IGA context creation failed: " + iga_status_message(create_status));
      return resolution;
    }

    const iga_disassemble_options_t options{
        sizeof(iga_disassemble_options_t), kIgaFormattingPrintPc, 0U, 0U, 0U, 0U};
    const Dwfl_Callbacks callbacks{};
    DwflCleanup dwfl{dwfl_begin(&callbacks)};
    if (dwfl.dwfl == nullptr) {
      resolution.warnings.push_back("libdw failed to create an offline DWARF session.");
      return resolution;
    }
    Dwfl_Module* module = dwfl_report_offline(
        dwfl.dwfl, request.mangled_kernel_name.c_str(), request.dwarf_file.c_str(), -1);
    if (module == nullptr || dwfl_report_end(dwfl.dwfl, nullptr, nullptr) != 0) {
      resolution.warnings.push_back("libdw failed to load the ELF/DWARF file: " +
                                    std::string(dwfl_errmsg(-1)));
      return resolution;
    }
    std::string dwarf_error;
    const std::optional<std::vector<DwarfLineLocation>> dwarf_locations =
        load_dwarf_line_locations(module, dwarf_error);
    if (!dwarf_locations.has_value()) {
      resolution.warnings.push_back(dwarf_error);
      return resolution;
    }

    std::size_t decoded_instruction_count = 0;
    for (std::size_t offset = 0; offset + 16U <= kernel_text->bytes.size(); offset += 16U) {
      char* disassembly = nullptr;
      const iga_status_t disassemble_status = iga_context_disassemble_instruction(
          context.context,
          &options,
          kernel_text->bytes.data() + offset,
          nullptr,
          nullptr,
          &disassembly);
      if (disassemble_status != IGA_SUCCESS) {
        continue;
      }
      ++decoded_instruction_count;
      if (const auto location = resolve_source_location(
              *dwarf_locations, request, offset, kernel_text->dwarf_address + offset)) {
        resolution.locations.push_back(*location);
      }
    }
    if (decoded_instruction_count == 0U) {
      resolution.warnings.push_back("IGA could not decode any 16-byte instruction boundary for this kernel.");
    }
    if (resolution.locations.empty()) {
      resolution.warnings.push_back("IGA decoded instructions, but libdw found no source locations.");
    }
    return resolution;
  }
};

}  // namespace

DwarfAdapterPtr make_iga_adapter() {
  return std::make_unique<IgaAdapter>();
}

}  // namespace dwarf_parser_check