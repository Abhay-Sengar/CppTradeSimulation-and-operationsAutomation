// util.hpp — tiny hot-path helpers.
#pragma once
#include <immintrin.h>  // _mm_pause

namespace ts {

// PAUSE inside a spin-wait (email §"Lock-Free": "_mm_pause() in spin loops —
// why it matters"). Three reasons:
//  1. It hints the core to de-pipeline the spin, cutting power/heat.
//  2. On an SMT core it yields issue slots to the sibling thread.
//  3. Crucially, it avoids a memory-order-violation pipeline flush: without
//     PAUSE the CPU speculatively loads the spin variable many times; when it
//     finally changes, the machine clears the pipeline (~tens of cycles).
//     PAUSE slows the spin just enough to dodge that penalty on wake.
inline void cpu_relax() noexcept { _mm_pause(); }

}  // namespace ts
