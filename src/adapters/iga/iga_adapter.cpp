#include "adapters/iga/iga_adapter.h"

#include <elfutils/libdw.h>
#include <elfutils/libdwfl.h>
#include <fcntl.h>
#include <gelf.h>
#include <libelf.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <unistd.h>

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

struct DwarfLineLocation {
  std::uint64_t address = 0;
  std::string file;
  std::uint64_t line = 0;
  std::optional<std::uint64_t> column;
};

std::optional<std::uint64_t> load_kernel_dwarf_address(
    const std::filesystem::path& path,
    const std::string& symbol_name,
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

  return kernel_symbol.st_value;
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

    if (request.runtime_kernel_address == 0U) {
      resolution.warnings.push_back("kernel debug JSON is missing runtime_kernel_address.");
      return resolution;
    }
    if (request.addr2line_ips.empty()) {
      resolution.warnings.push_back("no addr2line IPs were supplied for this kernel.");
      return resolution;
    }

    std::string load_error;
    const std::optional<std::uint64_t> dwarf_address =
        load_kernel_dwarf_address(request.dwarf_file, request.mangled_kernel_name, load_error);
    if (!dwarf_address.has_value()) {
      resolution.warnings.push_back(load_error);
      return resolution;
    }

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

    for (const std::uint64_t address : request.addr2line_ips) {
      const std::uint64_t offset = address - request.runtime_kernel_address;
      if (const auto location = resolve_source_location(
              *dwarf_locations, request, offset, *dwarf_address + offset)) {
        resolution.locations.push_back(*location);
      }
    }
    if (resolution.locations.empty()) {
      resolution.warnings.push_back("libdw found no source locations for the supplied addr2line IPs.");
    }
    return resolution;
  }
};

}  // namespace

DwarfAdapterPtr make_iga_adapter() {
  return std::make_unique<IgaAdapter>();
}

}  // namespace dwarf_parser_check