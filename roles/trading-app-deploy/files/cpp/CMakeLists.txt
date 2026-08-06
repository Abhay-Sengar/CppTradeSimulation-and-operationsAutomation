# ---------------------------------------------------------------------------
# CppTradeSimulation — trading engine + mock exchange
#
# Interview topics touched by this file (email §"Build & Tooling", §"Compiler
# & Build"): CMAKE_CXX_STANDARD 20, CMAKE_CXX_EXTENSIONS OFF, ccache, LTO,
# -O3 -march=native, warning flags, ASan+UBSan vs TSan sanitiser presets.
#
# Why a real CMake file (not a one-line g++): the role deploys with cmake, and
# "CMake fundamentals, build types, linking" is on the study list. Build with:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# Sanitiser builds (mutually exclusive — see SANITIZE below):
#   cmake -S . -B build-asan -DSANITIZE=address ...
#   cmake -S . -B build-tsan -DSANITIZE=thread  ...
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.16)
project(cpp_trade_sim CXX)

# --- language: C++20, no GNU extensions (email: CMAKE_CXX_EXTENSIONS OFF) -----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- ccache if present (email: "ccache") — transparent compile cache ---------
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
  message(STATUS "ccache: ${CCACHE_PROGRAM}")
endif()

# --- optional link-time optimisation (email: "LTO option") -------------------
option(ENABLE_LTO "Enable -flto" OFF)

# --- optional sanitiser: 'address' (ASan+UBSan) OR 'thread' (TSan) -----------
# They are mutually exclusive: ASan and TSan both hook the allocator / shadow
# memory and cannot coexist in one binary. ASan+UBSan catches use-after-free,
# OOB, leaks, and UB; TSan catches data races. Ship neither in the -O3 build.
set(SANITIZE "" CACHE STRING "address | thread | (empty for none)")

# --- warning + optimisation flags (email §Compiler & Build, §Build & Tooling)-
#  -O3            aggressive opt for the hot path
#  -march=native  emit the host ISA (here alderlake) incl. PAUSE for _mm_pause.
#                 NOTE on hybrid CPUs: GCC resolves native to the ISA the P- and
#                 E-cores have in COMMON, which is why AVX-512 is absent — the
#                 E-cores don't implement it, so the package doesn't expose it.
#  -fno-rtti      we use CRTP, never dynamic_cast; this is the "why not
#                 dynamic_cast on the hot path" answer, enforced by the build
#  -Wall -Wextra -Wconversion -Wshadow -Werror  the exact set from the email
# NOTE: exceptions are intentionally left ENABLED. The hot path is noexcept and
# allocation-free by discipline; -fno-exceptions program-wide would fight
# <future>/<thread> for no hot-path benefit. See INTERVIEW_NOTES.md.
add_library(trade_flags INTERFACE)
target_include_directories(trade_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(trade_flags INTERFACE
  -O3 -march=native -fno-rtti
  -Wall -Wextra -Wconversion -Wshadow -Werror)
target_link_libraries(trade_flags INTERFACE pthread)

if(ENABLE_LTO)
  target_compile_options(trade_flags INTERFACE -flto)
  target_link_options(trade_flags INTERFACE -flto)
endif()

if(SANITIZE STREQUAL "address")
  target_compile_options(trade_flags INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer -O1)
  target_link_options(trade_flags INTERFACE -fsanitize=address,undefined)
elseif(SANITIZE STREQUAL "thread")
  target_compile_options(trade_flags INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer -O1)
  target_link_options(trade_flags INTERFACE -fsanitize=thread)
endif()

# --- binaries ----------------------------------------------------------------
add_executable(trading_engine src/engine_main.cpp)
target_link_libraries(trading_engine PRIVATE trade_flags)

add_executable(mock_exchange src/exchange_main.cpp)
target_link_libraries(mock_exchange PRIVATE trade_flags)

# Standalone rdtsc micro-benchmarks (no external dep — always built).
add_executable(microbench bench/microbench.cpp)
target_link_libraries(microbench PRIVATE trade_flags)

# --- optional tests (GTest) — cut-line -------------------------------------
option(BUILD_TESTS "Build GTest unit tests" OFF)
if(BUILD_TESTS)
  find_package(GTest QUIET)
  if(GTest_FOUND)
    enable_testing()
    add_executable(unit_tests tests/test_spsc.cpp tests/test_hist.cpp)
    target_link_libraries(unit_tests PRIVATE trade_flags GTest::gtest_main)
    add_test(NAME unit_tests COMMAND unit_tests)
  else()
    message(WARNING "BUILD_TESTS=ON but GTest not found — skipping tests")
  endif()
endif()

# --- optional Google Benchmark version — cut-line --------------------------
# The standalone `microbench` target above needs no dependency and is the one
# used in the demo; this is the same idea expressed with Google Benchmark, built
# only if the library is present (email lists "GTest + Google Benchmark").
option(BUILD_GBENCH "Build the Google Benchmark variant" OFF)
if(BUILD_GBENCH)
  find_package(benchmark QUIET)
  if(benchmark_FOUND)
    add_executable(gbench bench/bench_gbench.cpp)
    target_link_libraries(gbench PRIVATE trade_flags benchmark::benchmark)
  else()
    message(WARNING "BUILD_GBENCH=ON but Google Benchmark not found — skipping")
  endif()
endif()
