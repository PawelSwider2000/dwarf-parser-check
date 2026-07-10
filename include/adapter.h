#pragma once

#include <memory>
#include <string>

#include "types.h"

namespace dwarf_parser_check {

class DwarfAdapter {
 public:
  virtual ~DwarfAdapter() = default;

  [[nodiscard]] virtual std::string name() const = 0;
  [[nodiscard]] virtual bool supports(const ResolveRequest& request) const = 0;
  [[nodiscard]] virtual KernelResolution resolve_kernel(const ResolveRequest& request) const = 0;
};

using DwarfAdapterPtr = std::unique_ptr<DwarfAdapter>;

}  // namespace dwarf_parser_check