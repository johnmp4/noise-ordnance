// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

class RingBuffer {
public:
  explicit RingBuffer(size_t capacity)
    : capacity_(nextPowerOfTwo(capacity)),
      buffer_(capacity_, 0.0f),
      read_(0),
      write_(0),
      size_(0)
  {
  }

  void reset() {
    read_ = 0;
    write_ = 0;
    size_ = 0;
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
  }

  bool push(const float* src, size_t frames) {
    if (frames > availableSpace())
      return false;

    for (size_t i = 0; i < frames; ++i) {
      buffer_[write_] = src[i];
      write_ = (write_ + 1) & (capacity_ - 1);
    }

    size_ += frames;
    return true;
  }

  size_t pop(float* dst, size_t frames) {
    const size_t toCopy = std::min(frames, size_);
    for (size_t i = 0; i < toCopy; ++i) {
      dst[i] = buffer_[read_];
      read_ = (read_ + 1) & (capacity_ - 1);
    }

    size_ -= toCopy;
    return toCopy;
  }

  size_t available() const { return size_; }
  size_t capacity() const { return capacity_; }
  size_t availableSpace() const { return capacity_ - size_; }

private:
  static size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value)
      power <<= 1;
    return power;
  }

  size_t capacity_;
  std::vector<float> buffer_;
  size_t read_;
  size_t write_;
  size_t size_;
};
