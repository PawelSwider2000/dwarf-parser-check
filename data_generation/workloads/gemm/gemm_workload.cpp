#include <sycl/sycl.hpp>

#include <cmath>
#include <iostream>
#include <vector>

#include "workload.h"

namespace {

constexpr int kMatrixSize = 1024;
constexpr int kTotalLoops = 30;
constexpr float kAValue = 0.128f;
constexpr float kBValue = 0.256f;
constexpr float kMaxEps = 1.0e-4f;

class PrimaryGEMMKernel;

void GEMM(const float *a, const float *b, float *c, unsigned size,
          sycl::id<2> id) {
  int row = id.get(0);
  int column = id.get(1);
  float sum = 0.0f;
  for (unsigned k = 0; k < size; ++k) {
    sum += a[row * size + k] * b[k * size + column];
  }
  c[row * size + column] = sum;
}

float CheckResults(const std::vector<float> &c, float expectedValue) {
  float eps = 0.0f;
  for (size_t i = 0; i < c.size(); ++i) {
    eps += std::fabs((c[i] - expectedValue) / expectedValue);
  }
  return eps / c.size();
}

bool RunGEMM(sycl::queue &queue, const std::vector<float> &a,
             const std::vector<float> &b, std::vector<float> &c,
             unsigned size, float expectedResult) {
  try {
    sycl::buffer<float, 1> aBuffer(a.data(), a.size());
    sycl::buffer<float, 1> bBuffer(b.data(), b.size());
    sycl::buffer<float, 1> cBuffer(c.data(), c.size());

    queue.submit([&](sycl::handler &handler) {
      auto aAccessor = aBuffer.get_access<sycl::access::mode::read>(handler);
      auto bAccessor = bBuffer.get_access<sycl::access::mode::read>(handler);
      auto cAccessor = cBuffer.get_access<sycl::access::mode::write>(handler);

      handler.parallel_for<PrimaryGEMMKernel>(sycl::range<2>(size, size),
                                              [=](sycl::id<2> id) {
        auto aPointer =
            aAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        auto bPointer =
            bAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        auto cPointer =
            cAccessor.template get_multi_ptr<sycl::access::decorated::no>();
        GEMM(aPointer.get(), bPointer.get(), cPointer.get(), size, id);
      });
    });
    queue.wait_and_throw();
  } catch (const sycl::exception &e) {
    std::cerr << "SYCL Exception: " << e.what() << "\n";
    return false;
  }

  float eps = CheckResults(c, expectedResult);
  bool passed = eps < kMaxEps;
  std::cout << "GEMM results are " << (passed ? "CORRECT" : "INCORRECT")
            << " (accuracy: " << eps << ")\n";
  return passed;
}

} // namespace

bool Workload(sycl::queue &queue) {
  const unsigned size = kMatrixSize;
  std::vector<float> a(static_cast<size_t>(size) * size, kAValue);
  std::vector<float> b(static_cast<size_t>(size) * size, kBValue);
  std::vector<float> c(static_cast<size_t>(size) * size, 0.0f);
  const float expectedResult = kAValue * kBValue * size;

  for (int iteration = 0; iteration < kTotalLoops; ++iteration) {
    std::cout << "[host] >>> submitting iteration " << iteration << "\n";
    std::cout << "[host] launching PrimaryGEMMKernel with matrix size "
              << size << "x" << size << "\n";
    if (!RunGEMM(queue, a, b, c, size, expectedResult)) {
      return false;
    }
  }
  return true;
}