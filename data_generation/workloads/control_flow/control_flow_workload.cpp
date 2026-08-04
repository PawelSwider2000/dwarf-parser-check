#include <sycl/sycl.hpp>

#include <cstdint>
#include <iostream>
#include <vector>

#include "workload.h"

namespace {

constexpr size_t kElementCount = 4096*4096;
constexpr int kTotalLoops = 30;
constexpr int kComputeIterations = 16;

class ArithmeticKernel;
class IfElseKernel;
class TernaryKernel;
class SwitchKernel;

int ComputeExpected(std::uint32_t result, int value) {
  for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
    result = result * 1664525U + 1013904223U;
    result ^= static_cast<std::uint32_t>(value + iteration);
  }
  return static_cast<int>(result & 0x7fffffffU);
}

int ArithmeticExpected(int value) {
  return ComputeExpected(value * 3 + 7, value);
}

int IfElseExpected(int value) {
  if (value < -2) {
    return ComputeExpected(value * value + 11, value);
  }
  if (value > 3) {
    return ComputeExpected(value * 5 - 4, value);
  }
  return ComputeExpected(value + 19, value);
}

int TernaryExpected(int value) {
  return ComputeExpected((value & 1) == 0 ? value * 2 : value * -3, value);
}

int SwitchExpected(int value) {
  switch ((value + 8) % 4) {
    case 0:
      return ComputeExpected(value + 31, value);
    case 1:
      return ComputeExpected(value * value, value);
    case 2:
      return ComputeExpected(value - 17, value);
    default:
      return ComputeExpected(value * -2 + 5, value);
  }
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

bool RunArithmeticKernel(sycl::queue &queue, const std::vector<int> &input,
                         std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<ArithmeticKernel>(sycl::range<1>(input.size()),
                                               [=](sycl::id<1> id) {
          const int value = inputAccessor[id];
          std::uint32_t result = value * 3 + 7;
          for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
            result = result * 1664525U + 1013904223U;
            result ^= static_cast<std::uint32_t>(value + iteration);
          }
          outputAccessor[id] = static_cast<int>(result & 0x7fffffffU); // MARK: arithmetic
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, ArithmeticExpected, "ArithmeticKernel");
}

bool RunIfElseKernel(sycl::queue &queue, const std::vector<int> &input,
                     std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<IfElseKernel>(sycl::range<1>(input.size()),
                                           [=](sycl::id<1> id) {
          const int value = inputAccessor[id];
          std::uint32_t result;
          if (value < -2) {
            result = value * value + 11; // MARK: if-body
            for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
              result = result * 1664525U + 1013904223U;
              result ^= static_cast<std::uint32_t>(value + iteration);
            }
          } else if (value > 3) {
            result = value * 5 - 4; // MARK: else-if-body
            for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
              result = result * 1664525U + 1013904223U;
              result ^= static_cast<std::uint32_t>(value + iteration);
            }
          } else {
            result = value + 19; // MARK: else-body
            for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
              result = result * 1664525U + 1013904223U;
              result ^= static_cast<std::uint32_t>(value + iteration);
            }
          }
          outputAccessor[id] = static_cast<int>(result & 0x7fffffffU);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, IfElseExpected, "IfElseKernel");
}

bool RunTernaryKernel(sycl::queue &queue, const std::vector<int> &input,
                      std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<TernaryKernel>(sycl::range<1>(input.size()),
                                            [=](sycl::id<1> id) {
          const int value = inputAccessor[id];
          std::uint32_t result = (value & 1) == 0 ? value * 2 : value * -3;
          for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
            result = result * 1664525U + 1013904223U;
            result ^= static_cast<std::uint32_t>(value + iteration);
          }
          outputAccessor[id] = static_cast<int>(result & 0x7fffffffU); // MARK: ternary
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, TernaryExpected, "TernaryKernel");
}

bool RunSwitchKernel(sycl::queue &queue, const std::vector<int> &input,
                     std::vector<int> &output) {
  try {
    {
      sycl::buffer<int, 1> inputBuffer(input.data(), input.size());
      sycl::buffer<int, 1> outputBuffer(output.data(), output.size());
      queue.submit([&](sycl::handler &handler) {
        auto inputAccessor = inputBuffer.get_access<sycl::access::mode::read>(handler);
        auto outputAccessor = outputBuffer.get_access<sycl::access::mode::write>(handler);
        handler.parallel_for<SwitchKernel>(sycl::range<1>(input.size()),
                                           [=](sycl::id<1> id) {
          const int value = inputAccessor[id];
          std::uint32_t result;
          switch ((value + 8) % 4) {
            case 0:
              result = value + 31; // MARK: switch-case-0
              for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
                result = result * 1664525U + 1013904223U;
                result ^= static_cast<std::uint32_t>(value + iteration);
              }
              break;
            case 1:
              result = value * value; // MARK: switch-case-1
              for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
                result = result * 1664525U + 1013904223U;
                result ^= static_cast<std::uint32_t>(value + iteration);
              }
              break;
            case 2:
              result = value - 17; // MARK: switch-case-2
              for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
                result = result * 1664525U + 1013904223U;
                result ^= static_cast<std::uint32_t>(value + iteration);
              }
              break;
            default:
              result = value * -2 + 5; // MARK: switch-default
              for (int iteration = 0; iteration < kComputeIterations; ++iteration) {
                result = result * 1664525U + 1013904223U;
                result ^= static_cast<std::uint32_t>(value + iteration);
              }
              break;
          }
          outputAccessor[id] = static_cast<int>(result & 0x7fffffffU);
        });
      });
      queue.wait_and_throw();
    }
  } catch (const sycl::exception &error) {
    std::cerr << "SYCL Exception: " << error.what() << "\n";
    return false;
  }
  return ValidateResults(input, output, SwitchExpected, "SwitchKernel");
}

} // namespace

bool Workload(sycl::queue &queue) {
  std::vector<int> input(kElementCount);
  std::vector<int> output(kElementCount);
  for (size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<int>(index % 17) - 8;
  }

  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    if (!RunArithmeticKernel(queue, input, output) ||
        !RunIfElseKernel(queue, input, output) ||
        !RunTernaryKernel(queue, input, output) ||
        !RunSwitchKernel(queue, input, output)) {
      return false;
    }
  }
  return true;
}