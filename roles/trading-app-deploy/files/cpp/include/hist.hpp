// hist.hpp — an HDR-style latency histogram with lock-free recording.
//
// Interview topics (email §"Profiling & Measurement"):
//   * "HDR histogram vs simple averages — why p99/p99.9 matter". A mean hides
//     the tail; in trading the tail IS the risk (a 99.9th-percentile stall is
//     the one that misses the market). We keep the full distribution, cheaply.
//   * HDR = High Dynamic Range: each power-of-two octave is split into kSub
//     linear sub-buckets, giving a *fixed relative precision* (~1/kSub) across
//     many orders of magnitude (ns to seconds) in a small fixed array. The
//     octave is found with __builtin_clzll (leading-zero count = bit length);
//     the sub-bucket is the next kSubBits bits below the leading 1. One bucket
//     per octave (plain log2) would be too coarse to separate, say, a 770ns
//     from an 800ns path — the sub-buckets are what make the A/B measurable.
//
// Concurrency: the gateway thread records; the metrics-http thread reads. Both
// use RELAXED atomics. That is correct here because the buckets are independent
// counters and percentiles only need an approximate, eventually-consistent
// snapshot — we deliberately do NOT pay for acquire/release or a lock on the
// hot path. (Contrast: the SPSC ring needs acquire/release because it publishes
// data alongside the index.)
#pragma once
#include "types.hpp"
#include <array>
#include <atomic>

namespace ts {

class Histogram {
public:
    static constexpr int kSubBits = 6;                  // 64 sub-buckets / octave
    static constexpr int kSub = 1 << kSubBits;          // => ~1.5% relative error
    static constexpr int kLinear = 2 * kSub;            // exact region [0, kLinear)
    static constexpr int kMaxOctave = 40;              // up to 2^40 ns (~18 min)
    static constexpr int kBuckets = kLinear + (kMaxOctave - (kSubBits + 1)) * kSub;

    // Map a value to its bucket. Small values are counted exactly (linear
    // region); larger values land in octave*kSub + sub_bucket.
    [[nodiscard]] static int bucket_of(u64 v) noexcept {
        if (v < static_cast<u64>(kLinear)) return static_cast<int>(v);
        int oct = 63 - __builtin_clzll(v);              // floor(log2 v), >= kSubBits+1
        if (oct >= kMaxOctave) oct = kMaxOctave - 1;
        const u64 sub = (v - (u64{1} << oct)) >> (oct - kSubBits);  // 0..kSub-1
        return kLinear + (oct - (kSubBits + 1)) * kSub + static_cast<int>(sub);
    }

    // Representative ns for a bucket: the midpoint of the sub-bucket's range.
    [[nodiscard]] static u64 value_of(int b) noexcept {
        if (b < kLinear) return static_cast<u64>(b);
        const int idx = b - kLinear;
        const int oct = kSubBits + 1 + idx / kSub;
        const int sub = idx % kSub;
        const u64 base = u64{1} << oct;
        const u64 width = u64{1} << (oct - kSubBits);
        return base + static_cast<u64>(sub) * width + width / 2;
    }

    void record(u64 ns) noexcept {
        buckets_[static_cast<std::size_t>(bucket_of(ns))]
            .fetch_add(1, std::memory_order_relaxed);
        count_.fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(ns, std::memory_order_relaxed);
        u64 cur = max_.load(std::memory_order_relaxed);
        while (ns > cur &&
               !max_.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {
            // cur is reloaded by compare_exchange_weak on failure
        }
    }

    struct Snapshot {
        u64 count{}, mean{}, p50{}, p99{}, p999{}, max{};
    };

    [[nodiscard]] Snapshot snapshot() const noexcept {
        std::array<u64, kBuckets> b{};
        u64 total = 0;
        for (int i = 0; i < kBuckets; ++i) {
            b[static_cast<std::size_t>(i)] = counts_load(i);
            total += b[static_cast<std::size_t>(i)];
        }
        Snapshot s;
        s.count = total;
        s.max = max_.load(std::memory_order_relaxed);
        if (total == 0) return s;
        s.mean = sum_.load(std::memory_order_relaxed) / total;
        s.p50 = percentile(b, total, 0.50);
        s.p99 = percentile(b, total, 0.99);
        s.p999 = percentile(b, total, 0.999);
        return s;
    }

    void reset() noexcept {
        for (auto& c : buckets_) c.store(0, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
        sum_.store(0, std::memory_order_relaxed);
        max_.store(0, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] u64 counts_load(int i) const noexcept {
        return buckets_[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }

    [[nodiscard]] static u64 percentile(const std::array<u64, kBuckets>& b,
                                        u64 total, double p) noexcept {
        // smallest bucket whose cumulative count reaches the p-th item
        const u64 target = static_cast<u64>(static_cast<double>(total) * p);
        u64 cum = 0;
        for (int i = 0; i < kBuckets; ++i) {
            cum += b[static_cast<std::size_t>(i)];
            if (cum >= target) return value_of(i);
        }
        return value_of(kBuckets - 1);
    }

    std::array<std::atomic<u64>, kBuckets> buckets_{};
    std::atomic<u64> count_{0};
    std::atomic<u64> sum_{0};
    std::atomic<u64> max_{0};
};

}  // namespace ts
