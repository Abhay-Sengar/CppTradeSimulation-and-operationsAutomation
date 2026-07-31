// strategy.hpp — the same trivial trading rule expressed TWO ways: CRTP
// (compile-time) and virtual (runtime), selectable at startup for an A/B.
//
// Interview topics (email §"Performance-Sensitive Design Patterns"):
//   * "Why virtual dispatch is banned on the hot path": a virtual call loads the
//     vtable pointer, loads the slot, then does an indirect branch the CPU
//     can't inline and often mispredicts — ~5-100ns plus i-cache pollution, and
//     it blocks inlining of the callee.
//   * "CRTP as virtual replacement — implement." The derived type is a template
//     parameter, so the call is resolved at compile time, fully inlined, zero
//     indirection. Same polymorphic *shape*, no runtime cost.
//
// Both share one decision function so the A/B compares ONLY the dispatch
// mechanism, not the logic.
#pragma once
#include "messages.hpp"
#include "config.hpp"

namespace ts {

// The actual (deliberately trivial) rule. Alternate side by order id; quote at
// the mid; fixed size. Marked noexcept + no allocation — hot-path clean.
[[nodiscard]] inline Order decide(const Tick& t, u64 clordid) noexcept {
    const Side side = (clordid & 1u) ? Side::Buy : Side::Sell;
    const i64 mid = (t.bid + t.ask) / 2;
    // fix_len/fix are filled in by the strategy stage after encoding.
    return Order{t.t0_tsc, clordid, t.sym_id, side, {0, 0, 0}, mid, kLimits.order_qty, 0, {}};
}

// ---- CRTP flavour ---------------------------------------------------------
// Base<Derived>::on_tick statically dispatches to Derived::rule via a
// static_cast — no vtable. The strategy loop is templated on Derived, so the
// whole call chain inlines.
template <class Derived>
struct StrategyCRTP {
    [[nodiscard]] Order on_tick(const Tick& t, u64 clordid) noexcept {
        return static_cast<Derived*>(this)->rule(t, clordid);
    }
};

struct AltStrategy final : StrategyCRTP<AltStrategy> {
    [[nodiscard]] Order rule(const Tick& t, u64 clordid) noexcept {
        return decide(t, clordid);
    }
};

// ---- virtual flavour ------------------------------------------------------
// Same interface through a vtable. `final` lets the compiler devirtualise IF it
// can prove the dynamic type — but when the loop calls through IStrategy& it
// generally cannot, which is exactly the cost we want to measure.
struct IStrategy {
    IStrategy() = default;
    IStrategy(const IStrategy&) = delete;
    IStrategy& operator=(const IStrategy&) = delete;
    virtual ~IStrategy() = default;
    [[nodiscard]] virtual Order on_tick(const Tick& t, u64 clordid) noexcept = 0;
};

struct AltStrategyV final : IStrategy {
    [[nodiscard]] Order on_tick(const Tick& t, u64 clordid) noexcept override {
        return decide(t, clordid);
    }
};

}  // namespace ts
