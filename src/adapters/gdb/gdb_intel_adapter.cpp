#include "adapters/gdb/gdb_intel_adapter.h"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace dwarf_parser_check {
namespace {

struct Addr2LineFrame {
  std::string function_name;
  std::string file;
  std::optional<std::uint64_t> line;
};

struct Addr2LineResult {
  std::uint64_t address = 0;
  std::vector<Addr2LineFrame> frames;
};

std::optional<std::filesystem::path> addr2line_path() {
  const char* configured_path = std::getenv("DPC_GDB_ADDR2LINE");
  if (configured_path != nullptr && *configured_path != '\0') {
    return std::filesystem::path(configured_path);
  }
#if defined(DPC_GDB_ADDR2LINE_DEFAULT_PATH)
  return std::filesystem::path(DPC_GDB_ADDR2LINE_DEFAULT_PATH);
#else
  return std::nullopt;
#endif
}

std::uint64_t canonicalize_gpu_address(std::uint64_t address) {
  constexpr std::uint64_t kAddressMask = (UINT64_C(1) << 48) - 1;
  constexpr std::uint64_t kSignBit = UINT64_C(1) << 47;
  return (address & kAddressMask) | ((address & kSignBit) != 0U ? ~kAddressMask : 0U);
}

std::optional<std::uint64_t> parse_address(const std::string& value) {
  if (value.size() < 3U || value[0] != '0' || value[1] != 'x') {
    return std::nullopt;
  }

  std::uint64_t address = 0;
  const auto [end, error] = std::from_chars(value.data() + 2, value.data() + value.size(), address, 16);
  if (error != std::errc() || end != value.data() + value.size()) {
    return std::nullopt;
  }
  return address;
}

Addr2LineFrame parse_frame(const std::string& function_name, std::string location) {
  constexpr std::string_view kDiscriminator = " (discriminator ";
  const std::size_t discriminator = location.find(kDiscriminator);
  if (discriminator != std::string::npos) {
    location.resize(discriminator);
  }

  Addr2LineFrame frame;
  frame.function_name = function_name;
  const std::size_t separator = location.rfind(':');
  if (separator == std::string::npos) {
    frame.file = std::move(location);
    return frame;
  }

  frame.file = location.substr(0, separator);
  std::uint64_t line = 0;
  const auto [end, error] = std::from_chars(
      location.data() + separator + 1, location.data() + location.size(), line);
  if (error == std::errc() && end == location.data() + location.size() && line != 0U) {
    frame.line = line;
  }
  return frame;
}

std::vector<Addr2LineResult> parse_addr2line_output(const std::string& output) {
  std::vector<Addr2LineResult> results;
  std::istringstream stream(output);
  std::string line;
  std::optional<Addr2LineResult> current;
  std::optional<std::string> pending_function;

  while (std::getline(stream, line)) {
    if (const auto address = parse_address(line)) {
      if (current.has_value()) {
        results.push_back(std::move(*current));
      }
      current = Addr2LineResult{*address, {}};
      pending_function.reset();
      continue;
    }
    if (!current.has_value()) {
      continue;
    }
    if (!pending_function.has_value()) {
      pending_function = line;
    } else {
      current->frames.push_back(parse_frame(*pending_function, line));
      pending_function.reset();
    }
  }

  if (current.has_value()) {
    results.push_back(std::move(*current));
  }
  return results;
}

bool write_addresses(const std::filesystem::path& input_path,
                     const std::vector<std::uint64_t>& addresses) {
  std::ofstream input(input_path);
  if (!input) {
    return false;
  }
  input << std::hex;
  for (const std::uint64_t address : addresses) {
    input << "0x" << address << '\n';
  }
  return static_cast<bool>(input);
}

std::optional<std::string> run_addr2line(const std::filesystem::path& executable,
                                         const ResolveRequest& request,
                                         const std::vector<std::uint64_t>& addresses,
                                         std::string& error) {
  char input_template[] = "/tmp/dpc-gdb-addr2line-XXXXXX";
  const int input_fd = mkstemp(input_template);
  if (input_fd < 0) {
    error = "failed to create addr2line input file";
    return std::nullopt;
  }
  close(input_fd);
  const std::filesystem::path input_path(input_template);
  const auto remove_input = [&input_path] { std::filesystem::remove(input_path); };
  if (!write_addresses(input_path, addresses)) {
    remove_input();
    error = "failed to write addr2line input addresses";
    return std::nullopt;
  }

  int output_pipe[2] = {};
  if (pipe(output_pipe) != 0) {
    remove_input();
    error = "failed to create addr2line output pipe";
    return std::nullopt;
  }

  const pid_t process = fork();
  if (process < 0) {
    close(output_pipe[0]);
    close(output_pipe[1]);
    remove_input();
    error = "failed to start addr2line";
    return std::nullopt;
  }
  if (process == 0) {
    const int child_input = open(input_path.c_str(), O_RDONLY);
    if (child_input < 0 || dup2(child_input, STDIN_FILENO) < 0 ||
        dup2(output_pipe[1], STDOUT_FILENO) < 0) {
      _exit(127);
    }
    close(child_input);
    close(output_pipe[0]);
    close(output_pipe[1]);
    std::array<char*, 10> argv = {
        const_cast<char*>(executable.c_str()),
        const_cast<char*>("-a"),
        const_cast<char*>("-f"),
        const_cast<char*>("-C"),
        const_cast<char*>("-i"),
        const_cast<char*>("-b"),
        const_cast<char*>("elf64-intelgt"),
        const_cast<char*>("-e"),
        const_cast<char*>(request.dwarf_file.c_str()),
        nullptr,
    };
    execv(executable.c_str(), argv.data());
    _exit(127);
  }

  close(output_pipe[1]);
  std::string output;
  std::array<char, 4096> buffer = {};
  ssize_t bytes_read = 0;
  while ((bytes_read = read(output_pipe[0], buffer.data(), buffer.size())) > 0) {
    output.append(buffer.data(), static_cast<std::size_t>(bytes_read));
  }
  close(output_pipe[0]);
  remove_input();

  int status = 0;
  if (waitpid(process, &status, 0) < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    error = "gdb addr2line exited unsuccessfully";
    return std::nullopt;
  }
  return output;
}

class GdbIntelAdapter final : public DwarfAdapter {
 public:
  [[nodiscard]] std::string name() const override {
    return "gdb-intel";
  }

  [[nodiscard]] bool supports(const ResolveRequest& request) const override {
    return !request.dwarf_file.empty();
  }

  [[nodiscard]] KernelResolution resolve_kernel(const ResolveRequest& request) const override {
    KernelResolution resolution;
    resolution.backend_name = name();
    resolution.kernel_name = request.kernel_name;

    const std::optional<std::filesystem::path> executable = addr2line_path();
    if (!executable.has_value()) {
      resolution.warnings.push_back("GDB addr2line is not configured; set DPC_GDB_ADDR2LINE.");
      return resolution;
    }
    if (!std::filesystem::is_regular_file(*executable)) {
      resolution.warnings.push_back("GDB addr2line executable does not exist: " + executable->string());
      return resolution;
    }
    if (request.runtime_kernel_address == 0U) {
      resolution.warnings.push_back("kernel debug JSON is missing runtime_kernel_address.");
      return resolution;
    }
    if (request.addr2line_ips.empty()) {
      resolution.warnings.push_back("no addr2line IPs were supplied for this kernel.");
      return resolution;
    }

    const std::uint64_t kernel_address = canonicalize_gpu_address(request.runtime_kernel_address);

    std::string error;
    const std::optional<std::string> output =
        run_addr2line(*executable, request, request.addr2line_ips, error);
    if (!output.has_value()) {
      resolution.warnings.push_back(std::move(error));
      return resolution;
    }

    for (const Addr2LineResult& result : parse_addr2line_output(*output)) {
      if (result.frames.empty()) {
        continue;
      }
      const Addr2LineFrame& primary = result.frames.front();
      if (primary.file.empty() || primary.file == "??" || !primary.line.has_value()) {
        continue;
      }

      SourceLocation location;
      location.location.kernel_name = request.kernel_name;
      location.location.ip = result.address - kernel_address;
      location.location.file = primary.file;
      location.location.line = primary.line;
      for (std::size_t index = 1; index < result.frames.size(); ++index) {
        const Addr2LineFrame& frame = result.frames[index];
        location.inline_chain.push_back(
            {frame.function_name, frame.file, frame.line, std::nullopt});
      }
      resolution.locations.push_back(std::move(location));
    }

    if (resolution.locations.empty()) {
      resolution.warnings.push_back("GDB addr2line found no source locations for this kernel.");
    }
    return resolution;
  }
};

}  // namespace

DwarfAdapterPtr make_gdb_intel_adapter() {
  return std::make_unique<GdbIntelAdapter>();
}

}  // namespace dwarf_parser_check