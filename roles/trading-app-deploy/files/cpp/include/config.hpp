// config.hpp — compile-time configuration.
//
// Interview topics (email §"Core C++ Language"): constexpr / consteval /
// constinit, std::string_view, if constexpr, enum class.
#pragma once
#include "types.hpp"
#include <array>
#include <string_view>

namespace ts {

// The symbol universe (ported from the original Python engine). string_view =
// a non-owning {ptr,len} view over the string literals in .rodata — no copies,
// no allocation. constexpr array => the whole table lives at compile time.
inline constexpr std::array<std::string_view, 6> kSymbols{
    "NIFTY", "BANKNIFTY", "RELIANCE", "TCS", "INFY", "HDFCBANK"};
inline constexpr u32 kNumSymbols = static_cast<u32>(kSymbols.size());

// consteval => MUST be evaluated at compile time. Ring capacities have to be
// powers of two (the SPSC ring static_asserts it); computing them in a consteval
// function documents that intent and guarantees no runtime cost.
consteval u64 ring_capacity() noexcept { return u64{1} << 12; }  // 4096 slots
consteval u64 pending_capacity() noexcept { return u64{1} << 12; }

inline constexpr u16 kMetricsPort = 8000;
inline constexpr u16 kExchangePort = 9001;

// constinit => guarantees this global is constant-initialised (at compile time),
// avoiding both the static-initialisation-order fiasco and a runtime init guard.
struct EngineLimits {
    u64 warmup_orders;   // exclude the first N from the reported histograms
    i64 base_price;      // starting mid, in ticks
    i64 order_qty;       // fixed size, in ticks
};
constinit inline EngineLimits kLimits{
    .warmup_orders = 500,
    .base_price = 100 * 100,  // 100.00
    .order_qty = 50 * 100,
};

}  // namespace ts
