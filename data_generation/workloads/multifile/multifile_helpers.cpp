#include "multifile_helpers.h"

SYCL_EXTERNAL int ScaleAndBias(int value) {
  return value * 5 + 13; // MARK: scale-and-bias-helper
}

SYCL_EXTERNAL int CenteredSquare(int value) {
  const int centered = value - 3;
  return centered * centered - 9; // MARK: centered-square-helper
}