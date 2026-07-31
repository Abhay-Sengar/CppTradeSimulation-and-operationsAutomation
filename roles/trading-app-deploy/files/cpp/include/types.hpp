// types.hpp — primitive aliases + strong money types + enums.
//
// Interview topics (email §"Performance-Sensitive Design Patterns",
// §"Core C++ Language"):
//   * int64_t for money, NEVER double — floating point can't represent 0.10
//     exactly (0.1 is a repeating binary fraction), so decimal prices and P&L
//     accumulate rounding error. We store price/qty as scaled integers.
//   * strong typedefs: Price and Qty are distinct types with identical layout,
//     so the compiler rejects `price + qty` — a class of bug that a bare
//     `int64_t` everywhere would allow. Zero runtime cost.
//   * enum class vs unscoped enum: scoped, fixed underlying type, no implicit
//     int conversion — Side can't be silently mixed with an int.
//   * explicit, [[nodiscard]], constexpr, noexcept, operator<=> (C++20).
#pragma once
#include <cstdint>
#include <compare>
#include <type_traits>

namespace ts {

// Fixed-width aliases used everywhere. Keeps struct layouts unambiguous across
// the wire and in the ring buffers.
using i64 = std::int64_t;
using u64 = std::uint64_t;
using u32 = std::uint32_t;
using u16 = std::uint16_t;
using u8  = std::uint8_t;

// Prices are stored as an integer number of ticks (here: paise, i.e. 1/100 of
// a rupee). PRICE_SCALE is the ticks-per-unit factor. consteval so it is a
// pure compile-time constant (email: consteval).
consteval i64 price_scale() noexcept { return 100; }

// Strong integer newtype. `Tag` makes Price and Qty distinct types even though
// both wrap an i64. Trivially copyable => usable directly inside POD wire/ring
// structs. Compiles down to a bare i64 (verify with -O3: no overhead).
template <class Tag>
struct Strong {
    i64 v{};

    constexpr Strong() noexcept = default;
    // explicit: forbid silent int -> Price conversions at call sites.
    constexpr explicit Strong(i64 x) noexcept : v{x} {}

    [[nodiscard]] constexpr i64 raw() const noexcept { return v; }

    // Same-type arithmetic only (Price+Price ok; Price+Qty won't compile).
    [[nodiscard]] constexpr Strong operator+(Strong o) const noexcept { return Strong{v + o.v}; }
    [[nodiscard]] constexpr Strong operator-(Strong o) const noexcept { return Strong{v - o.v}; }

    // C++20 three-way comparison — gives ==,!=,<,<=,>,>= for free.
    [[nodiscard]] constexpr auto operator<=>(const Strong&) const noexcept = default;
    [[nodiscard]] constexpr bool operator==(const Strong&) const noexcept = default;
};

struct PriceTag {};
struct QtyTag {};
using Price = Strong<PriceTag>;
using Qty   = Strong<QtyTag>;

static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_trivially_copyable_v<Qty>);
static_assert(sizeof(Price) == sizeof(i64), "Strong<> must be zero-overhead");

// FIX tag 54 values. Scoped enum, fixed to u8 so it packs tightly in wire and
// ring structs and never implicitly converts to int.
enum class Side : u8 { Buy = 1, Sell = 2 };

// Build a Price from a whole+fraction pair at compile time where possible,
// e.g. px(100, 50) == 100.50 -> 10050 ticks. constexpr: usable in constants.
[[nodiscard]] constexpr Price px(i64 whole, i64 paise = 0) noexcept {
    return Price{whole * price_scale() + paise};
}

}  // namespace ts
