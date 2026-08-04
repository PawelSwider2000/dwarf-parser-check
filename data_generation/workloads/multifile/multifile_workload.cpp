#include <sycl/sycl.hpp>

#include <iostream>
#include <vector>

#include "multifile_helpers.h"
#include "workload.h"

namespace {

constexpr size_t kElementCount = 4096;
constexpr int kTotalLoops = 30;

class ScaleAndBiasKernel;
class CenteredSquareKernel;

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

bool RunScaleAndBiasKernel(sycl::queue &queue, const std::vector<int> &input,
                           std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<ScaleAndBiasKernel>(sycl::range<1>(input.size()),
                                                 [=](sycl::id<1> id) {
          outputAccessor[id] = ScaleAndBias(inputAccessor[id]);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, ScaleAndBias, "ScaleAndBiasKernel");
}

bool RunCenteredSquareKernel(sycl::queue &queue, const std::vector<int> &input,
                            std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<CenteredSquareKernel>(sycl::range<1>(input.size()),
                                                  [=](sycl::id<1> id) {
          outputAccessor[id] = CenteredSquare(inputAccessor[id]);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, CenteredSquare, "CenteredSquareKernel");
}

} // namespace

bool Workload(sycl::queue &queue) {
  std::vector<int> input(kElementCount);
  std::vector<int> output(kElementCount);
  for (size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<int>(index % 31) - 15;
  }

  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    if (!RunScaleAndBiasKernel(queue, input, output) ||
        !RunCenteredSquareKernel(queue, input, output)) {
      return false;
    }
  }
  return true;
}