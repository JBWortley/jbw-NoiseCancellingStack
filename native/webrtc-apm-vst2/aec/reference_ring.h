// SPDX-License-Identifier: MIT
#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <vector>

namespace ncs {

// Single-producer/single-consumer ring of interleaved float samples.
//
// The producer is the audio engine's loopback thread (IApoAuxiliaryInputRT::AcceptInput) and the
// consumer is the capture processing thread (IAudioProcessingObjectRT::APOProcess). Windows does
// not synchronize those callbacks with each other, so after Allocate() neither side allocates,
// locks or waits. Positions are monotonic sample counters; each side writes only its own.
class ReferenceRing final {
 public:
  // Non-real-time. Must not run while either side is active.
  void Allocate(std::size_t capacity_samples) {
    buffer_.assign(capacity_samples, 0.0F);
    write_.store(0, std::memory_order_relaxed);
    read_.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t Capacity() const noexcept { return buffer_.size(); }

  // Producer. Writes the whole block or nothing; a full ring drops the newest block.
  bool Write(const float* samples, std::size_t count) noexcept { return Push(samples, count); }

  // Producer. Appends silence so a silent loopback buffer keeps the reference timeline intact.
  bool WriteSilence(std::size_t count) noexcept { return Push(nullptr, count); }

  // Consumer.
  [[nodiscard]] std::size_t Available() const noexcept {
    return write_.load(std::memory_order_acquire) - read_.load(std::memory_order_relaxed);
  }

  // Consumer. Reads the whole block or nothing.
  bool Read(float* destination, std::size_t count) noexcept {
    const std::size_t read = read_.load(std::memory_order_relaxed);
    const std::size_t available = write_.load(std::memory_order_acquire) - read;
    if (count == 0 || count > available || buffer_.empty()) {
      return false;
    }
    const std::size_t start = read % buffer_.size();
    const std::size_t first = std::min(count, buffer_.size() - start);
    std::memcpy(destination, buffer_.data() + start, first * sizeof(float));
    std::memcpy(destination + first, buffer_.data(), (count - first) * sizeof(float));
    read_.store(read + count, std::memory_order_release);
    return true;
  }

  // Consumer. Drops up to `count` of the oldest samples.
  void Discard(std::size_t count) noexcept {
    const std::size_t read = read_.load(std::memory_order_relaxed);
    const std::size_t available = write_.load(std::memory_order_acquire) - read;
    read_.store(read + std::min(count, available), std::memory_order_release);
  }

  // Consumer. Drops everything written so far.
  void DiscardAll() noexcept { read_.store(write_.load(std::memory_order_acquire), std::memory_order_release); }

 private:
  bool Push(const float* samples, std::size_t count) noexcept {
    const std::size_t write = write_.load(std::memory_order_relaxed);
    const std::size_t used = write - read_.load(std::memory_order_acquire);
    if (count == 0 || buffer_.empty() || count > buffer_.size() - used) {
      return false;
    }
    const std::size_t start = write % buffer_.size();
    const std::size_t first = std::min(count, buffer_.size() - start);
    if (samples != nullptr) {
      std::memcpy(buffer_.data() + start, samples, first * sizeof(float));
      std::memcpy(buffer_.data(), samples + first, (count - first) * sizeof(float));
    } else {
      std::memset(buffer_.data() + start, 0, first * sizeof(float));
      std::memset(buffer_.data(), 0, (count - first) * sizeof(float));
    }
    write_.store(write + count, std::memory_order_release);
    return true;
  }

  std::vector<float> buffer_;
  std::atomic<std::size_t> write_{0};
  std::atomic<std::size_t> read_{0};
};

}  // namespace ncs
