#include "multifile_inline_chain_helpers.h"

SYCL_EXTERNAL std::uint32_t ExternalSalt(std::uint32_t value, std::uint32_t salt) {
  value ^= salt + 0x27d4eb2dU;
  value = RotateLeft(value, 9);
  value *= 0x165667b1U; // MARK: external-salt
  return value ^ (value >> 13);
}