#include "multifile_helpers.h"

SYCL_EXTERNAL int HeaderKernelExpected(int value) {
  return value * value + value - 7; // MARK: extra-helper
}