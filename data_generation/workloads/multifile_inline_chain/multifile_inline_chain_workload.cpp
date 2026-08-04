#include <sycl/sycl.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

#include "multifile_inline_chain_helpers.h"
#include "workload.h"

namespace {

constexpr size_t kElementCount = 32 * 1024;
constexpr size_t kLocalSize = 256;
constexpr int kTotalLoops = 30;

class MainTransformKernel;
class LocalNeighborhoodKernel;
class ChainedReductionKernel;

inline std::uint32_t MainLeaf(std::uint32_t value, std::uint32_t salt) {
  value = HeaderBridge(value ^ salt, salt + 0x6d2b79f5U);
  return RotateLeft(value, 5) ^ (value >> 17); // MARK: main-leaf
}

inline std::uint32_t MainChain(std::uint32_t value, std::uint32_t salt) {
  std::uint32_t state = HeaderPipelineValue(value, salt);
  for (int round = 0; round < 16; ++round) {
    state = MainLeaf(state + static_cast<std::uint32_t>(round), salt ^ state);
    state = HeaderMiddle(state, salt + static_cast<std::uint32_t>(round));
  }
  return state; // MARK: main-chain
}

inline std::uint32_t NeighborhoodValue(std::uint32_t value, std::uint32_t neighbor,
                                       std::uint32_t index) {
  const std::uint32_t first = MainChain(value, index ^ 0x1234abcdU);
  const std::uint32_t second = HeaderPipelineValue(neighbor, index ^ 0x90abcdefU);
  return MainLeaf(first ^ RotateLeft(second, 3), index); // MARK: neighborhood-value
}

bool ValidateValues(const std::vector<int> &input, const std::vector<std::uint32_t> &output,
                    std::uint32_t (*expected)(std::uint32_t, std::uint32_t),
                    const char *kernelName) {
  for (size_t index = 0; index < input.size(); ++index) {
    const std::uint32_t expectedValue = expected(static_cast<std::uint32_t>(input[index]),
                                                 static_cast<std::uint32_t>(index));
    if (output[index] != expectedValue) {
      std::cerr << "[host] " << kernelName << " failed at index " << index
                << ": expected " << expectedValue << ", got " << output[index] << "\n";
      return false;
    }
  }
  std::cout << "[host] " << kernelName << " results are CORRECT\n";
  return true;
}

std::uint32_t HeaderExpected(std::uint32_t value, std::uint32_t index) {
  return HeaderPipelineValue(value, index);
}

std::uint32_t MainExpected(std::uint32_t value, std::uint32_t index) {
  return MainChain(value, index ^ 0x31415926U);
}

bool ValidateNeighborhoodValues(const std::vector<int> &input,
                                const std::vector<std::uint32_t> &output) {
  for (size_t index = 0; index < input.size(); ++index) {
    const size_t localBase = (index / kLocalSize) * kLocalSize;
    const size_t localIndex = index % kLocalSize;
    const size_t neighborIndex = localBase + ((localIndex + 17) % kLocalSize);
    const std::uint32_t expectedValue = NeighborhoodValue(
        static_cast<std::uint32_t>(input[index]),
        static_cast<std::uint32_t>(input[neighborIndex]), static_cast<std::uint32_t>(index));
    if (output[index] != expectedValue) {
      std::cerr << "[host] LocalNeighborhoodKernel failed at index " << index
                << ": expected " << expectedValue << ", got " << output[index] << "\n";
      return false;
    }
  }
  std::cout << "[host] LocalNeighborhoodKernel results are CORRECT\n";
  return true;
}

std::uint64_t ReductionExpected(const std::vector<int> &input) {
  std::uint64_t total = 0;
  for (size_t index = 0; index < input.size(); ++index) {
    total += MainChain(static_cast<std::uint32_t>(input[index]),
                       static_cast<std::uint32_t>(index) ^ 0xfedcba98U);
  }
  return total;
}

bool RunHeaderPipelineKernel(sycl::queue &queue, const std::vector<int> &input,
                             std::vector<std::uint32_t> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<std::uint32_t, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        SubmitHeaderPipelineKernel(handler, inputAccessor, outputAccessor, input.size());
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateValues(input, output, HeaderExpected, "HeaderPipelineKernel");
}

bool RunMainTransformKernel(sycl::queue &queue, const std::vector<int> &input,
                            std::vector<std::uint32_t> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<std::uint32_t, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<MainTransformKernel>(sycl::range<1>(input.size()),
                                                  [=](sycl::id<1> id) {
          outputAccessor[id] = MainChain(static_cast<std::uint32_t>(inputAccessor[id]),
                                         static_cast<std::uint32_t>(id[0]) ^ 0x31415926U);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateValues(input, output, MainExpected, "MainTransformKernel");
}

bool RunLocalNeighborhoodKernel(sycl::queue &queue, const std::vector<int> &input,
                                std::vector<std::uint32_t> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<std::uint32_t, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        sycl::local_accessor<std::uint32_t, 1> localValues(sycl::range<1>(kLocalSize),
                                                            handler);
        handler.parallel_for<LocalNeighborhoodKernel>(
            sycl::nd_range<1>(sycl::range<1>(input.size()), sycl::range<1>(kLocalSize)),
            [=](sycl::nd_item<1> item) {
              const size_t globalIndex = item.get_global_linear_id();
              const size_t localIndex = item.get_local_linear_id();
                localValues[localIndex] = static_cast<std::uint32_t>(inputAccessor[globalIndex]);
              item.barrier(sycl::access::fence_space::local_space);
              const std::uint32_t neighbor = localValues[(localIndex + 17) % kLocalSize];
              outputAccessor[globalIndex] = NeighborhoodValue(
                  static_cast<std::uint32_t>(inputAccessor[globalIndex]), neighbor,
                  static_cast<std::uint32_t>(globalIndex));
            });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateNeighborhoodValues(input, output);
}

bool RunChainedReductionKernel(sycl::queue &queue, const std::vector<int> &input) {
  std::uint64_t total = 0;
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<std::uint64_t, 1> totalBuffer(&total, sycl::range<1>(1));
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto reduction = sycl::reduction(totalBuffer, handler, sycl::plus<std::uint64_t>());
        handler.parallel_for<ChainedReductionKernel>(sycl::range<1>(input.size()), reduction,
                                                     [=](sycl::id<1> id, auto &sum) {
          sum += static_cast<std::uint64_t>(
              MainChain(static_cast<std::uint32_t>(inputAccessor[id]),
                        static_cast<std::uint32_t>(id[0]) ^ 0xfedcba98U));
        });
        /* Previous manual reduction implementation:
        auto totalAccessor = totalBuffer.get_access<sycl::access::mode::read_write>(handler);
        handler.parallel_for<ChainedReductionKernel>(sycl::range<1>(input.size()),
                               [=](sycl::id<1> id) {
          sycl::atomic_ref<std::uint64_t, sycl::memory_order::relaxed,
                   sycl::memory_scope::device,
                   sycl::access::address_space::global_space>
            sum(totalAccessor[0]);
          sum.fetch_add(static_cast<std::uint64_t>(
            MainChain(static_cast<std::uint32_t>(inputAccessor[id]),
                static_cast<std::uint32_t>(id[0]) ^ 0xfedcba98U)));
        });
        */
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  const std::uint64_t expected = ReductionExpected(input);
  if (total != expected) {
    std::cerr << "[host] ChainedReductionKernel failed: expected " << expected
              << ", got " << total << "\n";
    return false;
  }
  std::cout << "[host] ChainedReductionKernel results are CORRECT\n";
  return true;
}

} // namespace

bool Workload(sycl::queue &queue) {
  std::vector<int> input(kElementCount);
  std::vector<std::uint32_t> output(kElementCount);
  for (size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<int>(index % 257) - 128;
  }

  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    if (!RunHeaderPipelineKernel(queue, input, output) ||
        !RunMainTransformKernel(queue, input, output) ||
        !RunLocalNeighborhoodKernel(queue, input, output) ||
        !RunChainedReductionKernel(queue, input)) {
      return false;
    }
  }
  return true;
}