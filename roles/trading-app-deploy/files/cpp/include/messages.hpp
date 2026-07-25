// messages.hpp — the POD payloads that flow through the SPSC rings.
//
// Every ring payload is a plain, trivially-copyable struct: no pointers to
// owned memory, no vtables, no constructors that allocate. That is what lets
// the ring do a flat `slot = item` copy and lets us reason about lifetimes
// trivially (email §"POD / trivially-copyable struct discipline").
#pragma once
#include "types.hpp"
#include <type_traits>

namespace ts {

enum class LogType : u8 { OrderSent = 1, Fill = 2, Info = 3 };

// ring1: market-data -> strategy. t0_tsc is stamped at generation and RIDES the
// whole pipeline so tick-to-trade can be measured end to end.
struct Tick {
    u64 t0_tsc;   // rdtsc at generation — the T2T stopwatch start
    u32 sym_id;   // index into kSymbols
    u32 seq;      // per-symbol sequence, for the strategy rule
    i64 bid;      // scaled integer price (ticks)
    i64 ask;
};

// ring2: strategy -> gateway. Carries t0_tsc forward from the originating tick,
// plus the already-encoded FIX NewOrderSingle so the gateway does pure I/O. The
// strategy encodes into a per-order scratch object (see OrderCtx) that is
// allocated pool-vs-malloc for the A/B; the encoded bytes are then copied here
// and sent, so the allocation has an observable effect the optimiser can't elide.
struct Order {
    u64  t0_tsc;
    u64  clordid;  // monotonically increasing client order id
    u32  sym_id;
    Side side;
    u8   _pad[3];  // explicit padding — keep layout deterministic, no warnings
    i64  px;
    i64  qty;
    u32  fix_len;
    char fix[120];  // encoded NewOrderSingle, ready for send()
};

// ring3: gateway -> logger. Pre-formatted off the hot path; the logger just
// writes bytes. Fixed inline buffer => no owned heap in the ring.
struct LogEvent {
    u64     tsc;
    LogType type;
    u8      len;
    char    msg[54];
};

static_assert(std::is_trivially_copyable_v<Tick>);
static_assert(std::is_trivially_copyable_v<Order>);
static_assert(std::is_trivially_copyable_v<LogEvent>);

// Transient per-order scratch object. In a real engine this would hold richer
// order state; here it exists so the --alloc=pool|malloc A/B has something to
// allocate per iteration and expose malloc jitter on the tail.
struct OrderCtx {
    u64  clordid;
    u64  t0_tsc;
    char fix_buf[112];  // room for the encoded NewOrderSingle
    u32  fix_len;
};
static_assert(std::is_trivially_copyable_v<OrderCtx>);

}  // namespace ts
