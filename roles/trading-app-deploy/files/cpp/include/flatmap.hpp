// flatmap.hpp — open-addressing (linear-probing) flat hash map.
//
// Interview topics (email §"Performance-Sensitive Design Patterns"):
//   * "Flat containers: open-addressing hash maps (absl::flat_hash_map) vs
//     node-based". A flat map stores all keys+values in ONE contiguous array and
//     resolves collisions by probing the next slot — so a lookup is a couple of
//     cache-line reads with no pointer chasing and NO per-element allocation.
//   * "Why std::unordered_map is banned on the hot path": the standard mandates
//     separate chaining with a node allocated per element and reference
//     stability, i.e. every insert calls the allocator and every lookup chases
//     pointers through scattered nodes (cache-miss per hop).
//
// This is the artifact behind that talking point; the microbenchmark compares it
// head-to-head with std::unordered_map. (The engine's live pending-order store
// uses an even simpler direct-mapped array, since clordids are monotonic.)
#pragma once
#include "types.hpp"
#include <array>
#include <cstddef>

namespace ts {

template <class V, std::size_t Cap>
class FlatMap {
    static_assert((Cap & (Cap - 1)) == 0, "capacity must be a power of two");
    static constexpr std::size_t kMask = Cap - 1;

    struct Slot {
        u64  key{};
        V    val{};
        bool used{false};
    };

public:
    // Fibonacci hashing: multiply by 2^64/phi and take the top bits. Cheap, and
    // spreads sequential integer keys well (a plain modulo would cluster them).
    [[nodiscard]] static std::size_t hash(u64 k) noexcept {
        return static_cast<std::size_t>((k * 0x9E3779B97F4A7C15ull) >> 1) & kMask;
    }

    // Insert or overwrite. Returns false if full (never allocates, never grows).
    bool put(u64 key, const V& val) noexcept {
        std::size_t i = hash(key);
        for (std::size_t probe = 0; probe < Cap; ++probe) {
            Slot& s = slots_[i];
            if (!s.used || s.key == key) {
                if (!s.used) ++size_;
                s.key = key;
                s.val = val;
                s.used = true;
                return true;
            }
            i = (i + 1) & kMask;  // linear probe: next slot, same cache line first
        }
        return false;
    }

    [[nodiscard]] V* get(u64 key) noexcept {
        std::size_t i = hash(key);
        for (std::size_t probe = 0; probe < Cap; ++probe) {
            Slot& s = slots_[i];
            if (!s.used) return nullptr;      // empty slot => not present
            if (s.key == key) return &s.val;
            i = (i + 1) & kMask;
        }
        return nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    std::array<Slot, Cap> slots_{};
    std::size_t size_{0};
};

}  // namespace ts
