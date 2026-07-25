// bump.hpp — arena / bump allocator (email §"Memory & Allocation": implement).
//
// The simplest possible allocator: hand out slices of one contiguous block by
// advancing an offset. Allocation is a pointer bump + alignment round-up — a
// handful of instructions, no free-list, no metadata. Individual objects are
// never freed; you reset() the whole arena at a natural boundary (e.g. end of a
// batch / trading session). Perfect for many small, same-lifetime allocations.
#pragma once
#include "types.hpp"
#include <cstddef>
#include <cstdint>

namespace ts {

class BumpAllocator {
public:
    BumpAllocator(void* base, std::size_t size) noexcept
        : base_(static_cast<std::uint8_t*>(base)), size_(size) {}

    // Bump-allocate `n` bytes with `align` alignment (power of two). Returns
    // nullptr when the arena is exhausted — never grows, never calls malloc.
    [[nodiscard]] void* alloc(std::size_t n, std::size_t align = alignof(std::max_align_t)) noexcept {
        const std::size_t cur = reinterpret_cast<std::uintptr_t>(base_ + off_);
        const std::size_t aligned = (cur + (align - 1)) & ~(align - 1);
        const std::size_t new_off = (aligned - reinterpret_cast<std::uintptr_t>(base_)) + n;
        if (new_off > size_) [[unlikely]] return nullptr;
        off_ = new_off;
        return base_ + (aligned - reinterpret_cast<std::uintptr_t>(base_));
    }

    template <class T, class... Args>
    [[nodiscard]] T* make(Args&&... args) noexcept {
        void* p = alloc(sizeof(T), alignof(T));
        return p ? ::new (p) T(static_cast<Args&&>(args)...) : nullptr;
    }

    void reset() noexcept { off_ = 0; }
    [[nodiscard]] std::size_t used() const noexcept { return off_; }

private:
    std::uint8_t* base_;
    std::size_t   size_;
    std::size_t   off_{0};
};

}  // namespace ts
