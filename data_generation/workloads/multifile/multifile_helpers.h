#pragma once

#include <sycl/sycl.hpp>

class HeaderImplementedKernel;

SYCL_EXTERNAL int ScaleAndBias(int value);
SYCL_EXTERNAL int CenteredSquare(int value);
SYCL_EXTERNAL int HeaderKernelExpected(int value);

template <typename InputAccessor, typename OutputAccessor>
void SubmitHeaderImplementedKernel(sycl::handler &handler, InputAccessor input,
																	OutputAccessor output, size_t elementCount) {
	handler.parallel_for<HeaderImplementedKernel>(sycl::range<1>(elementCount),
																								[=](sycl::id<1> id) {
		output[id] = HeaderKernelExpected(input[id]); // MARK: header-kernel
	});
}