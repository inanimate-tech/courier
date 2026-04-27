#ifndef COURIER_SPSC_QUEUE_H
#define COURIER_SPSC_QUEUE_H

#include <atomic>
#include <cstddef>

// Lock-free single-producer / single-consumer ring buffer.
//
// The transport task pushes; the main loop pops. Only the producer writes
// _head and only the consumer writes _tail, so an acquire/release pair on
// the indices is sufficient — no kernel primitives, no platform ifdefs,
// identical semantics on ESP32 and host.
//
// Capacity is the user-visible item count. Internally we reserve one extra
// slot so that head == tail unambiguously means "empty".
template <typename T, std::size_t Capacity>
class CourierSpscQueue {
    static_assert(Capacity > 0, "Capacity must be positive");
    static constexpr std::size_t N = Capacity + 1;

public:
    bool push(const T& value) {
        const std::size_t h = _head.load(std::memory_order_relaxed);
        const std::size_t next = (h + 1) % N;
        if (next == _tail.load(std::memory_order_acquire)) return false;
        _slots[h] = value;
        _head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(T& out) {
        const std::size_t t = _tail.load(std::memory_order_relaxed);
        if (t == _head.load(std::memory_order_acquire)) return false;
        out = _slots[t];
        _tail.store((t + 1) % N, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return _tail.load(std::memory_order_acquire) ==
               _head.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() { return Capacity; }

private:
    T _slots[N]{};
    std::atomic<std::size_t> _head{0};
    std::atomic<std::size_t> _tail{0};
};

#endif // COURIER_SPSC_QUEUE_H
