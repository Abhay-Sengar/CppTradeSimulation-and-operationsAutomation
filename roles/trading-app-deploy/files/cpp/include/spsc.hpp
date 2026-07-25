// spsc.hpp — single-producer / single-consumer lock-free ring buffer.
//
// This is the core lock-free structure the email asks to "implement from
// scratch" (§"Lock-Free Data Structures"). One template, three instances in
// the engine (market-data->strategy, strategy->gateway, gateway->logger).
//
// Design points, each an interview talking point:
//   * Power-of-two capacity + bitmask wrap (idx & (Cap-1)) instead of a modulo
//     — a single AND, no division.
//   * head_ and tail_ each on their OWN 64-byte cache line (alignas(64)) so the
//     producer (writes tail_) and consumer (writes head_) never invalidate each
//     other's line. That is the fix for FALSE SHARING.
//   * acquire/release only — no seq_cst, no locks. The producer's release-store
//     to tail_ "publishes" the slot it just wrote; the consumer's acquire-load
//     of tail_ "sees" that write. Each side loads its OWN index relaxed (only it
//     writes that index).
//   * cached opposite index: the producer keeps a private copy of head_ (and
//     vice-versa) so it only pays a cross-core atomic load when the ring looks
//     full/empty — otherwise it spins entirely on its own cache line.
//
// Why SPSC and not MPSC/MPMC (email: "when to use SPSC vs MPSC vs MPMC"):
// SPSC needs no CAS at all — plain loads/stores — because there is exactly one
// writer of each index. MPSC/MPMC need CAS loops (contention, ABA concerns);
// MPMC is a last resort. Our pipeline is a chain of SPSC hops by construction.
#pragma once
#include <atomic>
#include <array>
#include <cstddef>
#include <type_traits>

namespace ts {

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "ring payload must be trivially copyable");
    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscRing() = default;
    // Rule of 5: a ring wired between two threads must never be copied or moved
    // (the atomics and the in-flight indices are not relocatable). Delete them
    // explicitly rather than relying on the atomics to make it happen silently.
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;
    SpscRing(SpscRing&&) = delete;
    SpscRing& operator=(SpscRing&&) = delete;
    ~SpscRing() = default;

    // Producer side. Returns false if full (caller decides to spin/drop).
    [[nodiscard]] bool try_push(const T& item) noexcept {
        const std::size_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == Capacity) {                 // maybe full per cache
            head_cache_ = head_.load(std::memory_order_acquire);  // refresh once
            if (t - head_cache_ == Capacity) return false;        // really full
        }
        buf_[t & kMask] = item;                            // write the slot ...
        tail_.store(t + 1, std::memory_order_release);     // ... then publish it
        return true;
    }

    // Consumer side. Returns false if empty.
    [[nodiscard]] bool try_pop(T& out) noexcept {
        const std::size_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {                            // maybe empty per cache
            tail_cache_ = tail_.load(std::memory_order_acquire);  // refresh once
            if (h == tail_cache_) return false;                   // really empty
        }
        out = buf_[h & kMask];                             // read the slot ...
        head_.store(h + 1, std::memory_order_release);     // ... then free it
        return true;
    }

    [[nodiscard]] std::size_t size_approx() const noexcept {
        return tail_.load(std::memory_order_relaxed) -
               head_.load(std::memory_order_relaxed);
    }

private:
    // Producer's line: the tail counter it owns + its cached view of head.
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::size_t head_cache_{0};
    // Consumer's line: the head counter it owns + its cached view of tail.
    alignas(64) std::atomic<std::size_t> head_{0};
    std::size_t tail_cache_{0};
    // The storage, on its own line(s) away from the hot indices.
    alignas(64) std::array<T, Capacity> buf_{};
};

}  // namespace ts
