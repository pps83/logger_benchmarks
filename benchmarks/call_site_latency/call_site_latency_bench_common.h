#pragma once
#include <chrono>
#include <cstdint>
#ifndef _WIN32
  #include <pthread.h>
#endif

#if defined(_WIN32)
  #include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
  #include <x86intrin.h>
#endif
#include <algorithm>
#include <array>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>

// The finest timer resolution for every logger's process, as bte's common library sets at load
inline void set_finest_timer_resolution()
{
  using SetTimerResolution = LONG(NTAPI*)(ULONG, BOOLEAN, PULONG);
  auto const set = reinterpret_cast<SetTimerResolution>(
    GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetTimerResolution"));
  if (set != nullptr)
  {
    ULONG actual = 0;
    set(1, TRUE, &actual);
  }
  // Win11 drops a background process's raised resolution unless it opts out
  PROCESS_POWER_THROTTLING_STATE throttling{};
  throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
}

inline bool const finest_timer_resolution_set = (set_finest_timer_resolution(), true);
#endif

// perf c2c record -g --call-graph dwarf,8192  ./benchmark_quill_call_site_latency
// perf c2c report -NN -g --call-graph -c pid,iaddr --stdio
#define BENCH_WITHOUT_PERF

// A machine with fewer cores (a 4-core VM) sets the backend thread's core and BENCH_ONE_THREAD
#ifndef BENCH_BACKEND_CPU
  #define BENCH_BACKEND_CPU 5
#endif
#ifdef BENCH_ONE_THREAD
  #define THREAD_LIST_COUNT std::vector<int32_t>{1}
#elif defined(BENCH_THREADS)
  // The producer thread counts to run, such as BENCH_THREADS=1,2,4
  #define THREAD_LIST_COUNT std::vector<int32_t>{BENCH_THREADS}
#else
  #define THREAD_LIST_COUNT std::vector<int32_t>{1, 4}
#endif

#define ITERATIONS std::size_t{10'000}

// Total messages emitted per iteration across all logging threads. Keeping this
// aggregate fixed makes the 1-thread and 4-thread latency results comparable.
#define MESSAGES_PER_ITERATION std::size_t{20}

// Give the backend time to catch up between batches. This benchmark measures
// call-site latency under controlled load; sustained throughput is measured by
// the backend_total_time benchmarks.
// The pause is nearly all of a run's time; the backend drains a batch in microseconds, so a shorter one still works
#ifndef BENCH_WAIT_US
  #define BENCH_WAIT_US 200
#endif
#define MIN_WAIT_DURATION std::chrono::microseconds{BENCH_WAIT_US}
#define MAX_WAIT_DURATION std::chrono::microseconds{BENCH_WAIT_US + BENCH_WAIT_US / 10}

// #define BENCH_INT_INT_DOUBLE
// #define BENCH_INT_INT_LARGESTR

#if defined(__APPLE__)
// macOS has no way to pin a thread to a core
inline void set_pthread_affinity(pthread_t, int) {}
#elif !defined(_WIN32)
inline void set_pthread_affinity(pthread_t thread, int cpu)
{
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(cpu, &cpus);
  if (::pthread_setaffinity_np(thread, sizeof(cpus), &cpus) != 0)
    abort();
}
#endif

// The counter the measurements read: the TSC on x86, the generic timer (CNTVCT_EL0) on ARM64
inline uint64_t read_ticks() noexcept
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  return __rdtsc();
#elif defined(__aarch64__)
  uint64_t ticks;
  __asm__ volatile("mrs %0, cntvct_el0" : "=r"(ticks));
  return ticks;
#elif defined(_M_ARM64)
  return static_cast<uint64_t>(_ReadStatusReg(ARM64_SYSREG(3, 3, 14, 0, 2)));
#else
  #error no tick counter for this CPU
#endif
}

// Neither counter read serializes: fences on both sides keep the measured logging calls inside the timed region
inline uint64_t serialized_rdtsc() noexcept
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_lfence();
  uint64_t const timestamp = read_ticks();
  _mm_lfence();
#elif defined(__aarch64__)
  __asm__ volatile("isb" ::: "memory");
  uint64_t const timestamp = read_ticks();
  __asm__ volatile("isb" ::: "memory");
#else
  __isb(_ARM64_BARRIER_SY);
  uint64_t const timestamp = read_ticks();
  __isb(_ARM64_BARRIER_SY);
#endif
  return timestamp;
}

// The spin-wait hint: PAUSE on x86, YIELD on ARM64
inline void spin_pause() noexcept
{
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
  _mm_pause();
#elif defined(__aarch64__)
  __asm__ volatile("yield");
#else
  __yield();
#endif
}

/** -------- **/
inline double ns_per_rdtsc_tick()
{
  // Convert rdtsc to wall time.
  // 1. Get real time and rdtsc current count
  // 2. Calculate how many rdtsc ticks can occur in one
  // calculate _ticks_per_ns as the median over a number of observations.
  constexpr std::chrono::milliseconds spin_duration = std::chrono::milliseconds{10};

  constexpr int trials = 13;
  std::array<double, trials> rates = {{0}};

  for (size_t i = 0; i < trials; ++i)
  {
    auto const beg_ts =
      std::chrono::nanoseconds{std::chrono::steady_clock::now().time_since_epoch().count()};
    uint64_t const beg_tsc = serialized_rdtsc();

    std::chrono::nanoseconds elapsed_ns;
    uint64_t end_tsc;
    do
    {
      auto const end_ts =
        std::chrono::nanoseconds{std::chrono::steady_clock::now().time_since_epoch().count()};
      end_tsc = serialized_rdtsc();

      elapsed_ns = end_ts - beg_ts; // calculates ns between two timespecs
    } while (elapsed_ns < spin_duration); // busy spin for 10ms

    rates[i] = static_cast<double>(end_tsc - beg_tsc) / static_cast<double>(elapsed_ns.count());
  }

  std::nth_element(rates.begin(), rates.begin() + trials / 2, rates.end());

  double const ticks_per_ns = rates[trials / 2];
  return 1 / ticks_per_ns;
}
