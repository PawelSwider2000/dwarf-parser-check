#include <sycl/sycl.hpp>

#include <iostream>
#include <vector>

#include "workload.h"

namespace {

constexpr size_t kElementCount = 4096*4096;
constexpr int kTotalLoops = 30;

class ArithmeticKernel;
class IfElseKernel;
class TernaryKernel;
class SwitchKernel;

int ArithmeticExpected(int value) {
  return value * 3 + 7;
}

int IfElseExpected(int value) {
  if (value < -2) {
    return value * value + 11;
  }
  if (value > 3) {
    return value * 5 - 4;
  }
  return value + 19;
}

int TernaryExpected(int value) {
  return (value & 1) == 0 ? value * 2 : value * -3;
}

int SwitchExpected(int value) {
  switch ((value + 8) % 4) {
    case 0:
      return value + 31;
    case 1:
      return value * value;
    case 2:
      return value - 17;
    default:
      return value * -2 + 5;
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
          outputAccessor[id] = value * 3 + 7; // MARK: arithmetic
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
          int result;
          if (value < -2) {
            result = value * value + 11; // MARK: if-body
          } else if (value > 3) {
            result = value * 5 - 4; // MARK: else-if-body
          } else {
            result = value + 19; // MARK: else-body
          }
          outputAccessor[id] = result;
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
          outputAccessor[id] =
              (value & 1) == 0 ? value * 2 : value * -3; // MARK: ternary
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
          int result;
          switch ((value + 8) % 4) {
            case 0:
              result = value + 31; // MARK: switch-case-0
              break;
            case 1:
              result = value * value; // MARK: switch-case-1
              break;
            case 2:
              result = value - 17; // MARK: switch-case-2
              break;
            default:
              result = value * -2 + 5; // MARK: switch-default
              break;
          }
          outputAccessor[id] = result;
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