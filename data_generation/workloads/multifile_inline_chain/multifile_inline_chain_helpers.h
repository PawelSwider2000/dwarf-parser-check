#pragma once

#include <cstddef>
#include <cstdint>

#include <sycl/sycl.hpp>

#include "multifile_inline_chain_math.h"

class HeaderPipelineKernel;

SYCL_EXTERNAL std::uint32_t ExternalSalt(std::uint32_t value, std::uint32_t salt);

inline std::uint32_t HeaderBridge(std::uint32_t value, std::uint32_t salt) {
  const std::uint32_t mixed = HeaderChain(value, salt);
  return ExternalSalt(mixed, salt ^ 0xa511e9b3U); // MARK: header-bridge
}

inline std::uint32_t HeaderPipelineValue(std::uint32_t value, std::uint32_t salt) {
  std::uint32_t state = HeaderBridge(value, salt);
  for (int round = 0; round < 12; ++round) {
    state = HeaderMiddle(state ^ static_cast<std::uint32_t>(round), salt + state);
  }
  return state; // MARK: header-pipeline
}

template <typename InputAccessor, typename OutputAccessor>
void SubmitHeaderPipelineKernel(sycl::handler &handler, InputAccessor input,
                                OutputAccessor output, size_t elementCount) {
  handler.parallel_for<HeaderPipelineKernel>(sycl::range<1>(elementCount),
                                             [=](sycl::id<1> id) {
    output[id] = HeaderPipelineValue(static_cast<std::uint32_t>(input[id]),
                                     static_cast<std::uint32_t>(id[0]));
  });
}