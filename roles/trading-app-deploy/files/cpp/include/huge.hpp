// huge.hpp — a huge-page-backed, pre-faulted, memory-locked arena.
//
// Interview topics (email §"Memory & Allocation"):
//   * mmap(MAP_HUGETLB): back the hot state with explicit 2MB huge pages. One
//     TLB entry covers 2MB instead of 512x4KB pages, so the hot working set
//     causes far fewer TLB misses (each miss is a page-table walk).
//   * Explicit huge pages vs THP: Transparent Huge Pages are assembled lazily by
//     khugepaged and can be split/collapsed at unpredictable times — that
//     jitter is unacceptable on the hot path, so we set THP=never and reserve
//     explicit huge pages up front. (email: "why THP is worse than explicit".)
//   * MAP_POPULATE: pre-fault every page at mmap() time so the first hot-path
//     touch never takes a minor page fault. madvise(MADV_WILLNEED) reinforces.
//   * mlockall(MCL_CURRENT|MCL_FUTURE): pin all pages in RAM so nothing is ever
//     paged to swap mid-trade. Needs RLIMIT_MEMLOCK (systemd LimitMEMLOCK=
//     infinity) or CAP_IPC_LOCK.
#pragma once
#include "types.hpp"
#include <sys/mman.h>
#include <cstddef>
#include <cstdint>

#ifndef MAP_HUGETLB
#define MAP_HUGETLB 0x40000
#endif
#ifndef MAP_HUGE_SHIFT
#define MAP_HUGE_SHIFT 26
#endif

namespace ts {

inline constexpr std::size_t kHugePageSize = std::size_t{2} << 20;  // 2 MB

// Lock current + future pages into RAM. Returns false if denied (e.g. dev run
// without CAP_IPC_LOCK) — the engine warns and continues.
[[nodiscard]] inline bool lock_memory() noexcept {
    return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

class HugeArena {
public:
    // If `huge` is true, try MAP_HUGETLB (2MB pages); on failure fall back to a
    // normal anonymous mapping so `--huge=off` (or a box with no reserved huge
    // pages) still runs. Either way MAP_POPULATE pre-faults the whole region.
    HugeArena(std::size_t bytes, bool huge) noexcept {
        size_ = round_up(bytes, kHugePageSize);
        int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;
        if (huge) {
            const int hflags = flags | MAP_HUGETLB | (21 << MAP_HUGE_SHIFT);  // 2^21 = 2MB
            void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, hflags, -1, 0);
            if (p != MAP_FAILED) {
                base_ = p;
                huge_ok_ = true;
            }
        }
        if (base_ == nullptr) {
            void* p = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE, flags, -1, 0);
            base_ = (p == MAP_FAILED) ? nullptr : p;
        }
        if (base_ != nullptr) ::madvise(base_, size_, MADV_WILLNEED);
    }
    ~HugeArena() {
        if (base_ != nullptr) ::munmap(base_, size_);
    }
    HugeArena(const HugeArena&) = delete;
    HugeArena& operator=(const HugeArena&) = delete;

    // Bump-allocate from the arena (used to place the hot state at startup).
    [[nodiscard]] void* alloc(std::size_t n, std::size_t align) noexcept {
        const std::uintptr_t cur = reinterpret_cast<std::uintptr_t>(base_) + off_;
        const std::uintptr_t aligned = (cur + (align - 1)) & ~(std::uintptr_t{align} - 1);
        const std::size_t new_off = static_cast<std::size_t>(
            aligned - reinterpret_cast<std::uintptr_t>(base_)) + n;
        if (base_ == nullptr || new_off > size_) return nullptr;
        off_ = new_off;
        return reinterpret_cast<void*>(aligned);
    }

    [[nodiscard]] bool ok() const noexcept { return base_ != nullptr; }
    [[nodiscard]] bool using_hugepages() const noexcept { return huge_ok_; }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }

private:
    [[nodiscard]] static std::size_t round_up(std::size_t v, std::size_t a) noexcept {
        return (v + (a - 1)) & ~(a - 1);
    }
    void*       base_{nullptr};
    std::size_t size_{0};
    std::size_t off_{0};
    bool        huge_ok_{false};
};

}  // namespace ts
