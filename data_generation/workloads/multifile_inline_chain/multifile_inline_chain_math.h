#pragma once

#include <cstdint>

inline std::uint32_t RotateLeft(std::uint32_t value, int shift) {
  return (value << shift) | (value >> (32 - shift));
}

inline std::uint32_t HeaderLeaf(std::uint32_t value, std::uint32_t salt) {
  value ^= salt + 0x9e3779b9U;
  value *= 0x85ebca6bU; // MARK: header-leaf
  return RotateLeft(value, 11);
}

inline std::uint32_t HeaderMiddle(std::uint32_t value, std::uint32_t salt) {
  value = HeaderLeaf(value + 0x7f4a7c15U, salt);
  value ^= value >> 15; // MARK: header-middle
  return value * 0xc2b2ae35U;
}

inline std::uint32_t HeaderChain(std::uint32_t value, std::uint32_t salt) {
  std::uint32_t state = value ^ salt;
  for (int round = 0; round < 24; ++round) {
    state = HeaderMiddle(state, salt + static_cast<std::uint32_t>(round));
    state ^= RotateLeft(state, 7);
  }
  return state; // MARK: header-chain
}