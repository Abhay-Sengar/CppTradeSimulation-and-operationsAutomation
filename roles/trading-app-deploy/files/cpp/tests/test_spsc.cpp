// test_spsc.cpp — GTest unit tests for the SPSC ring (email: GTest).
#include "spsc.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>

using namespace ts;

TEST(Spsc, EmptyThenFifo) {
    SpscRing<int, 8> r;
    int out = -1;
    EXPECT_FALSE(r.try_pop(out));               // starts empty
    for (int i = 0; i < 8; ++i) EXPECT_TRUE(r.try_push(i));  // holds Capacity items
    EXPECT_FALSE(r.try_push(99));               // now full
    for (int i = 0; i < 8; ++i) {
        EXPECT_TRUE(r.try_pop(out));
        EXPECT_EQ(out, i);                       // FIFO order preserved
    }
    EXPECT_FALSE(r.try_pop(out));               // empty again
}

TEST(Spsc, ThreadedProducerConsumer) {
    constexpr int N = 1'000'000;
    SpscRing<int, 1024> r;
    std::atomic<long> sum{0};

    std::thread consumer([&] {
        int got = 0, v = 0;
        while (got < N) {
            if (r.try_pop(v)) { sum += v; ++got; }
        }
    });
    for (int i = 0; i < N; ++i) {
        while (!r.try_push(i)) { /* spin until space */ }
    }
    consumer.join();

    // Sum of 0..N-1 — proves nothing was lost or duplicated across the two cores.
    const long expected = static_cast<long>(N) * (N - 1) / 2;
    EXPECT_EQ(sum.load(), expected);
}
