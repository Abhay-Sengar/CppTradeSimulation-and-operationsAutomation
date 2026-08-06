// affinity.hpp — pin a thread to a core and (optionally) run it real-time.
//
// Interview topics (email §"Linux Systems"):
//   * pthread_setaffinity_np: bind a thread to one logical CPU so it never
//     migrates (migration blows the L1/L2 working set and the branch history).
//   * SCHED_FIFO: a real-time policy — a FIFO thread runs until it yields/blocks
//     and is never preempted by normal (SCHED_OTHER) tasks. Combined with an
//     isolated core (isolcpus/nohz_full) the hot thread owns the core outright.
//     Needs CAP_SYS_NICE (systemd AmbientCapabilities=CAP_SYS_NICE).
//   * RAII (email §"Memory Management"): AffinityGuard saves the current mask on
//     construction and restores it on scope exit — the same pattern used for
//     locks/files, here applied to an OS scheduler setting.
//
// SAFETY: a SCHED_FIFO thread that busy-spins will starve anything else on its
// core. Always pin FIRST, verify with taskset, and only then elevate to FIFO —
// so a mis-set affinity can never lock up a housekeeping core. The engine does
// exactly this and keeps SSH + non-hot threads on housekeeping cores.
#pragma once
#include "types.hpp"
#include <pthread.h>
#include <sched.h>

namespace ts {

// Bind the calling thread to `cpu`. Returns false on failure (e.g. the cpu is
// offline or not permitted).
[[nodiscard]] inline bool pin_to_cpu(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(static_cast<std::size_t>(cpu), &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
}

// Confirm the thread is actually running only on `cpu` (call after pin, before
// going real-time). Returns true iff the mask is exactly {cpu}.
[[nodiscard]] inline bool affinity_is(int cpu) noexcept {
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np(pthread_self(), sizeof(set), &set) != 0) return false;
    return CPU_ISSET(static_cast<std::size_t>(cpu), &set) && CPU_COUNT(&set) == 1;
}

// Set the thread's kernel-visible name, so /proc/<pid>/task/<tid>/comm, `top -H`
// and `htop` show "strategy" instead of five copies of "trading_engine" — which
// is what makes the thread-to-core pinning auditable from outside the process
// (see RUNBOOK.md §7). Limit is 15 chars + NUL; the kernel rejects anything
// longer, so callers keep the names short. Called once at thread start, never on
// the hot path.
inline void name_thread(const char* name) noexcept {
    static_cast<void>(pthread_setname_np(pthread_self(), name));
}

// Elevate to SCHED_FIFO at `prio` (1..99). Returns false if not permitted.
[[nodiscard]] inline bool set_realtime(int prio) noexcept {
    sched_param p{};
    p.sched_priority = prio;
    return pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) == 0;
}

// RAII guard: restore the original affinity mask when it goes out of scope.
class AffinityGuard {
public:
    AffinityGuard() noexcept {
        ok_ = pthread_getaffinity_np(pthread_self(), sizeof(saved_), &saved_) == 0;
    }
    ~AffinityGuard() {
        if (ok_) pthread_setaffinity_np(pthread_self(), sizeof(saved_), &saved_);
    }
    AffinityGuard(const AffinityGuard&) = delete;
    AffinityGuard& operator=(const AffinityGuard&) = delete;

private:
    cpu_set_t saved_{};
    bool      ok_{false};
};

}  // namespace ts
