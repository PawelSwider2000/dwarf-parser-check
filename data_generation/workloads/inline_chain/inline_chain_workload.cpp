#include <sycl/sycl.hpp>

#include <iostream>
#include <vector>

#include "workload.h"

namespace {

constexpr size_t kElementCount = 4096;
constexpr int kTotalLoops = 30;

class InlineChainAddKernel;
class InlineChainMultiplyKernel;

inline int AddLeaf(int value) {
  return value + 17; // MARK: add-leaf
}

inline int AddMiddle(int value) {
  return AddLeaf(value * 2); // MARK: add-middle
}

inline int AddRoot(int value) {
  return AddMiddle(value - 5); // MARK: add-root
}

inline int MultiplyLeaf(int value) {
  return value * 7; // MARK: multiply-leaf
}

inline int MultiplyMiddle(int value) {
  return MultiplyLeaf(value + 4); // MARK: multiply-middle
}

inline int MultiplyRoot(int value) {
  return MultiplyMiddle(value - 9); // MARK: multiply-root
}

bool ValidateResults(const std::vector<int> &input, const std::vector<int> &output,
                     int (*expected)(int), const char *kernelName) {
  for (size_t index = 0; index < input.size(); ++index) {
    if (output[index] != expected(input[index])) {
      std::cerr << "[host] " << kernelName << " failed at index " << index
                << ": expected " << expected(input[index]) << ", got "
                << output[index] << "\n";
      return false;
    }
  }
  std::cout << "[host] " << kernelName << " results are CORRECT\n";
  return true;
}

bool RunAddKernel(sycl::queue &queue, const std::vector<int> &input,
                  std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<InlineChainAddKernel>(sycl::range<1>(input.size()),
                                                   [=](sycl::id<1> id) {
          outputAccessor[id] = AddRoot(inputAccessor[id]);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, AddRoot, "InlineChainAddKernel");
}

bool RunMultiplyKernel(sycl::queue &queue, const std::vector<int> &input,
                       std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<InlineChainMultiplyKernel>(sycl::range<1>(input.size()),
                                                        [=](sycl::id<1> id) {
          outputAccessor[id] = MultiplyRoot(inputAccessor[id]);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, MultiplyRoot, "InlineChainMultiplyKernel");
}

} // namespace

bool Workload(sycl::queue &queue) {
  std::vector<int> input(kElementCount);
  std::vector<int> output(kElementCount);
  for (size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<int>(index % 29) - 14;
  }

  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    if (!RunAddKernel(queue, input, output) ||
        !RunMultiplyKernel(queue, input, output)) {
      return false;
    }
  }
  return true;
}