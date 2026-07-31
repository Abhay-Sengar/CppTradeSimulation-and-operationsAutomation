// test_hist.cpp — GTest unit tests for the HDR histogram (email: GTest).
#include "hist.hpp"
#include <gtest/gtest.h>

using namespace ts;

TEST(Hist, PercentilesApprox) {
    Histogram h;
    for (u64 i = 1; i <= 1000; ++i) h.record(i);  // uniform 1..1000
    const Histogram::Snapshot s = h.snapshot();
    EXPECT_EQ(s.count, 1000u);
    // HDR sub-buckets give ~1.5% relative error; 5% tolerance is comfortable.
    EXPECT_NEAR(static_cast<double>(s.p50), 500.0, 500.0 * 0.05);
    EXPECT_NEAR(static_cast<double>(s.p99), 990.0, 990.0 * 0.05);
    EXPECT_GE(s.max, 1000u);
    EXPECT_NEAR(static_cast<double>(s.mean), 500.0, 500.0 * 0.05);
}

TEST(Hist, SubBucketResolvesSmallDelta) {
    // The whole point of sub-buckets: 770ns and 800ns must NOT collapse into one
    // bucket (a plain one-per-octave log histogram would merge them).
    const int b770 = Histogram::bucket_of(770);
    const int b800 = Histogram::bucket_of(800);
    EXPECT_NE(b770, b800);
    EXPECT_LT(Histogram::value_of(b770), Histogram::value_of(b800));
}

TEST(Hist, ExactLinearRegion) {
    // Small values are counted exactly (linear region), no bucketing error.
    for (int v = 0; v < Histogram::kLinear; ++v)
        EXPECT_EQ(Histogram::value_of(Histogram::bucket_of(static_cast<u64>(v))),
                  static_cast<u64>(v));
}
