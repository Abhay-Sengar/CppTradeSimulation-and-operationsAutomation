// pool.hpp — fixed-capacity object pool with an intrusive free-list.
//
// Interview topics (email §"Memory & Allocation", §"Memory Management"):
//   * "Object pool with pointer free-list — implement." Pre-allocate N slots
//     once; acquire()/release() are O(1) and touch NO allocator. This is the
//     answer to "why new/malloc is banned on the hot path": a general allocator
//     can jitter from 50ns to 50us (lock contention, arena refill, page faults).
//   * Intrusive free-list: while a slot is free we reuse its own storage to hold
//     the "next free" pointer (a union). Zero per-slot overhead — no side table.
//   * Placement new + explicit destructor call: construct/destroy an object in
//     pre-owned raw storage without allocating.
#pragma once
#include "types.hpp"
#include <cstddef>
#include <new>
#include <utility>

namespace ts {

template <class T, std::size_t N>
class ObjectPool {
    // A slot is EITHER a free-list link OR live object storage — never both —
    // so a union costs us nothing.
    union Slot {
        Slot* next;                               // used while the slot is free
        alignas(T) unsigned char storage[sizeof(T)];  // used while it is live
        Slot() noexcept : next(nullptr) {}
        ~Slot() {}
    };

public:
    ObjectPool() noexcept {
        // Thread every slot onto the free list, front to back.
        for (std::size_t i = 0; i + 1 < N; ++i) slots_[i].next = &slots_[i + 1];
        slots_[N - 1].next = nullptr;
        free_ = &slots_[0];
    }
    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    // Construct a T in a recycled slot. Returns nullptr if the pool is drained
    // (caller decides what to do — never blocks, never allocates).
    template <class... Args>
    [[nodiscard]] T* acquire(Args&&... args) noexcept {
        if (free_ == nullptr) [[unlikely]] return nullptr;
        Slot* s = free_;
        free_ = s->next;
        return ::new (static_cast<void*>(s->storage)) T(std::forward<Args>(args)...);
    }

    void release(T* p) noexcept {
        if (p == nullptr) [[unlikely]] return;
        p->~T();  // explicit destructor — pairs with the placement new above
        // p points at Slot::storage, which is at offset 0 of the union, so the
        // address is exactly the Slot address: recover the link and re-list it.
        Slot* s = reinterpret_cast<Slot*>(p);
        s->next = free_;
        free_ = s;
    }

    [[nodiscard]] bool empty() const noexcept { return free_ == nullptr; }

private:
    Slot  slots_[N];
    Slot* free_{nullptr};
};

}  // namespace ts
