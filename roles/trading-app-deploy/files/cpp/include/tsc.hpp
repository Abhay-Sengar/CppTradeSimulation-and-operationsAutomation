// tsc.hpp — reading and calibrating the CPU timestamp counter (rdtsc).
//
// Interview topics (email §"Profiling & Measurement", §"Linux Systems"):
//   * rdtsc: a ~few-cycle read of a per-core 64-bit counter — far cheaper than
//     clock_gettime (which may be a vDSO call or a syscall). Ideal for the hot
//     path where we timestamp every tick.
//   * Calibration: rdtsc counts *cycles*, not nanoseconds. We measure how many
//     ticks elapse over a known CLOCK_MONOTONIC_RAW interval to get ns/tick.
//     MONOTONIC_RAW (not MONOTONIC) so NTP slewing doesn't distort the ratio.
//   * constant_tsc / nonstop_tsc (email: "/proc/cpuinfo flags"): "invariant
//     TSC". constant_tsc => the counter ticks at a fixed rate regardless of
//     P-state/turbo; nonstop_tsc => it keeps running in deep C-states. Together
//     they also mean the TSC is synchronised across cores, so subtracting a
//     tick stamped on cpu8 from one read on cpu10 is valid — which is exactly
//     what the cross-core tick-to-trade measurement does.
//   * Hybrid CPUs (this box is an Alder Lake i7-1255U, P-cores + E-cores): the
//     obvious worry is whether a counter read on an E-core is comparable to one
//     read on a P-core, since the two run at different clocks. It is — the TSC
//     is NOT the core clock. It is derived from the shared platform crystal at a
//     fixed ratio and is common to every core in the package — here the 38.4 MHz
//     crystal x 68 = 2611.2 MHz, whichever core reads it, while the cores
//     themselves range over 0.4-4.7 GHz. So one calibration is valid
//     engine-wide and cross-core deltas stay meaningful even across a P/E
//     boundary. The core clock varying underneath is exactly what constant_tsc
//     promises to ignore.
//     Sanity check on calibrate(): the kernel independently reports
//     "tsc: Detected 2611.200 MHz TSC" (journalctl -k | grep tsc) and this code
//     measures 2.611 GHz — agreement to 4 significant figures.
//   * Serialisation: a bare rdtsc can be reordered by the out-of-order engine.
//     rdtscp waits for prior instructions; the trailing lfence stops later
//     instructions hoisting above the read. Used where we need a precise fence;
//     the hot path uses the cheaper unfenced read where a few cycles of skew is
//     acceptable.
#pragma once
#include "types.hpp"
#include <x86intrin.h>  // __rdtsc, __rdtscp, _mm_lfence
#include <ctime>
#include <fstream>
#include <string>

namespace ts {

// Precisely fenced read — use at calibration boundaries.
[[nodiscard]] inline u64 rdtsc_serialized() noexcept {
    unsigned aux;
    _mm_lfence();
    const u64 t = __rdtscp(&aux);
    _mm_lfence();
    return t;
}

// Cheap unfenced read — use on the hot path (per-tick timestamp).
[[nodiscard]] inline u64 rdtsc_fast() noexcept { return __rdtsc(); }

// True iff /proc/cpuinfo advertises both invariant-TSC flags. Not hot-path.
[[nodiscard]] inline bool tsc_is_invariant() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("flags", 0) == 0) {
            const bool c = line.find("constant_tsc") != std::string::npos;
            const bool n = line.find("nonstop_tsc") != std::string::npos;
            return c && n;
        }
    }
    return false;
}

// Converts TSC deltas to nanoseconds using a calibrated ratio.
class TscClock {
public:
    // Calibrate over `sample_ms` of wall time against CLOCK_MONOTONIC_RAW.
    // Done once at startup (engine runs it on a std::async worker so the two
    // ~100ms calibration windows overlap other setup).
    [[nodiscard]] static TscClock calibrate(u32 sample_ms = 100) {
        timespec w0{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &w0);
        const u64 c0 = rdtsc_serialized();

        timespec req{0, static_cast<long>(sample_ms) * 1'000'000L};
        nanosleep(&req, nullptr);

        const u64 c1 = rdtsc_serialized();
        timespec w1{};
        clock_gettime(CLOCK_MONOTONIC_RAW, &w1);

        const i64 ns = (w1.tv_sec - w0.tv_sec) * 1'000'000'000LL +
                       (w1.tv_nsec - w0.tv_nsec);
        const u64 dticks = c1 - c0;
        TscClock clk;
        clk.ns_per_tick_ = static_cast<double>(ns) / static_cast<double>(dticks);
        return clk;
    }

    [[nodiscard]] u64 ticks_to_ns(u64 dticks) const noexcept {
        return static_cast<u64>(static_cast<double>(dticks) * ns_per_tick_);
    }
    [[nodiscard]] double ghz() const noexcept { return 1.0 / ns_per_tick_; }
    [[nodiscard]] double ns_per_tick() const noexcept { return ns_per_tick_; }

private:
    double ns_per_tick_{0.0};
};

}  // namespace ts
