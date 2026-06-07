#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

// Vyukov bounded MPSC queue — multiple producers, single consumer, fixed capacity.
template <typename T, size_t Capacity> class MpscRingBuffer {
  static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of 2");

  struct Cell {
    std::atomic<size_t> sequence;
    T data;
  };

  static constexpr size_t kMask = Capacity - 1;

  alignas(64) std::atomic<size_t> enqueue_pos_{0};
  alignas(64) std::atomic<size_t> dequeue_pos_{0};
  Cell buffer_[Capacity];

public:
  MpscRingBuffer() {
    for (size_t i = 0; i < Capacity; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  MpscRingBuffer(const MpscRingBuffer &) = delete;
  MpscRingBuffer &operator=(const MpscRingBuffer &) = delete;

  bool try_enqueue(const T &item) {
    Cell *cell = nullptr;
    size_t pos = enqueue_pos_.load(std::memory_order_relaxed);

    for (;;) {
      cell = &buffer_[pos & kMask];
      const size_t seq = cell->sequence.load(std::memory_order_acquire);
      const intptr_t dif = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

      if (dif == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1,   std::memory_order_relaxed)) {
          break;
        }
      } else if (dif < 0) {
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }

    cell->data = item;
    cell->sequence.store(pos + 1, std::memory_order_release);
    return true;
  }

  bool try_dequeue(T &item) {
    const size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    Cell *cell = &buffer_[pos & kMask];
    const size_t seq = cell->sequence.load(std::memory_order_acquire);
    const intptr_t dif =
        static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

    if (dif == 0) {
      item = cell->data;
      cell->sequence.store(pos + kMask + 1, std::memory_order_release);
      dequeue_pos_.store(pos + 1, std::memory_order_release);
      return true;
    }

    return false;
  }

  size_t size() const {
    const size_t head = dequeue_pos_.load(std::memory_order_acquire);
    const size_t tail = enqueue_pos_.load(std::memory_order_acquire);
    return tail - head;
  }

  bool empty() const { return size() == 0; }
};
