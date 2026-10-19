#pragma once

#include "../call_site_latency/call_site_latency_bench_common.h"

#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <thread>
#include <vector>

// Arguments: threads [exit|abort|crash <us after the signal> [more lines per thread]]; a late thread logs in a join run
using FirstLogFunc = void (*)(int thread, int line);

struct FirstLogRun
{
  FirstLogFunc log = nullptr;
  int lines = 0;
  std::atomic<int> ready{0};
  std::atomic<int> logged{0};
  std::atomic<bool> go{false};
  std::atomic<bool> release{false};
  uint64_t signal = 0;
  std::vector<uint64_t> wakes;
  std::vector<uint64_t> durations;
};

inline void log_first_in_burst(FirstLogRun& run, int thread)
{
  run.ready.fetch_add(1);
  while (!run.go.load(std::memory_order_acquire))
  {
    _mm_pause();
  }

  uint64_t const start = serialized_rdtsc();
  run.log(thread, 0);
  uint64_t const end = serialized_rdtsc();
  run.wakes[thread] = start - run.signal;
  run.durations[thread] = end - start;
  for (int line = 1; line <= run.lines; ++line)
  {
    run.log(thread, line);
  }
  run.logged.fetch_add(1);

  while (!run.release.load(std::memory_order_acquire))
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

inline void log_first_late(FirstLogRun const& run, int thread, uint64_t& duration)
{
  uint64_t const start = serialized_rdtsc();
  run.log(thread, 0);
  duration = serialized_rdtsc() - start;
}

inline double first_log_ns_per_tick()
{
  auto const begin = std::chrono::steady_clock::now();
  uint64_t const begin_tick = serialized_rdtsc();
  while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds{10})
  {
  }
  auto const elapsed = std::chrono::steady_clock::now() - begin;
  uint64_t const ticks = serialized_rdtsc() - begin_tick;
  return static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()) /
    static_cast<double>(ticks);
}

[[noreturn]] inline void end_process(char const* how)
{
  if (strcmp(how, "abort") == 0)
  {
    abort();
  }
  if (strcmp(how, "crash") == 0)
  {
    int* volatile nowhere = nullptr;
    *nowhere = 0;
  }
  exit(0);
}

inline unsigned long long ticks_to_ns(uint64_t ticks, double ns_per_tick)
{
  return static_cast<unsigned long long>(static_cast<double>(ticks) * ns_per_tick);
}

inline int run_first_log(int argc, char* argv[], FirstLogFunc log)
{
  int const thread_count = argc > 1 ? atoi(argv[1]) : 1;
  char const* const end = argc > 2 ? argv[2] : nullptr;
  double const end_delay_us = argc > 3 ? atof(argv[3]) : 0;
  int const lines = argc > 4 ? atoi(argv[4]) : 0;

#ifdef _WIN32
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
  double const ns_per_tick = first_log_ns_per_tick();

  FirstLogRun run;
  run.log = log;
  run.lines = lines;
  run.wakes.resize(thread_count);
  run.durations.resize(thread_count);
  std::vector<std::thread> threads;
  threads.reserve(thread_count);
  for (int thread = 0; thread < thread_count; ++thread)
  {
    threads.emplace_back(log_first_in_burst, std::ref(run), thread);
  }
  while (run.ready.load() != thread_count)
  {
    _mm_pause();
  }

  run.signal = serialized_rdtsc();
  run.go.store(true, std::memory_order_release);
  if (end != nullptr)
  {
    uint64_t const end_at = run.signal + static_cast<uint64_t>(end_delay_us * 1000 / ns_per_tick);
    while (serialized_rdtsc() < end_at)
    {
      _mm_pause();
    }
    end_process(end);
  }

  while (run.logged.load() != thread_count)
  {
    std::this_thread::sleep_for(std::chrono::microseconds{100});
  }
  // Time for the logger thread to get ready for a new thread, as it would between thread starts
  std::this_thread::sleep_for(std::chrono::milliseconds{10});
  uint64_t late = 0;
  std::thread(log_first_late, std::cref(run), thread_count, std::ref(late)).join();

  run.release.store(true, std::memory_order_release);
  for (std::thread& thread : threads)
  {
    thread.join();
  }

  std::string line = "first_log threads " + std::to_string(thread_count) + " wake_ns";
  for (uint64_t wake : run.wakes)
  {
    line += " " + std::to_string(ticks_to_ns(wake, ns_per_tick));
  }
  line += " log_ns";
  for (uint64_t duration : run.durations)
  {
    line += " " + std::to_string(ticks_to_ns(duration, ns_per_tick));
  }
  line += " late_ns " + std::to_string(ticks_to_ns(late, ns_per_tick)) + "\n";
  fwrite(line.data(), 1, line.size(), stderr);
  return 0;
}
