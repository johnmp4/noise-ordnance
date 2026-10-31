// Copyright (c) 2026 John Peterman
// SPDX-License-Identifier: MIT

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>

// Single-producer / single-consumer ring buffer for float audio.
// SPSC-safe: the producer owns write_/size_ increments, the consumer
// owns read_/size_ decrements, and size_ is atomic, so the audio thread
// and the DF worker thread can share two rings (audio -> worker input,
// worker -> audio output) without a mutex on the audio thread. The
// data writes happen-before the size increment (sequenced, atomic
// size_) and the size load happens-before the data reads, so the
// consumer never reads an uncommitted sample and the producer never
// overwrites an unread one (a push that would overflow returns false
// instead of overwriting -- the caller decides what dropping means).
// reset() is NOT thread-safe: call it only with no worker running
// (init / setModel / reset all stop the worker first).
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
    const size_t toCopy = std::min(frames, size_.load());
    for (size_t i = 0; i < toCopy; ++i) {
      dst[i] = buffer_[read_];
      read_ = (read_ + 1) & (capacity_ - 1);
    }

    size_ -= toCopy;
    return toCopy;
  }

  size_t available() const { return size_.load(); }
  size_t capacity() const { return capacity_; }
  size_t availableSpace() const { return capacity_ - size_.load(); }

private:
  static size_t nextPowerOfTwo(size_t value) {
    size_t power = 1;
    while (power < value)
      power <<= 1;
    return power;
  }

  size_t capacity_;
  std::vector<float> buffer_;
  size_t read_;   // consumer-owned (worker pops input, audio pops output)
  size_t write_;  // producer-owned (audio pushes input, worker pushes output)
  std::atomic<size_t> size_;
};
