#pragma once

#include "call_site_latency_bench_common.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#ifdef BENCH_WARM_VECTOR
  #include <immintrin.h>
#endif
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#else
  #include <sched.h>
#endif

// Instead of sleep
inline void wait([[maybe_unused]] std::chrono::nanoseconds min,
                 std::chrono::nanoseconds max,
                 [[maybe_unused]] size_t stream_id)
{
#ifdef BENCH_WITHOUT_PERF
  // Use a separate, repeatable sequence per producer so every logger sees the
  // same pacing without artificially synchronizing all producer threads.
  thread_local std::mt19937 gen{
    static_cast<std::mt19937::result_type>(0x51ee'50a7u + stream_id)};
  std::uniform_int_distribution<int64_t> dis(min.count(), max.count());

  auto const start_time = std::chrono::steady_clock::now();
  auto const end_time = start_time + std::chrono::nanoseconds{dis(gen)};
  std::chrono::nanoseconds time_now;
  #ifdef BENCH_WARM_VECTOR
  // A 256-bit op each turn keeps the core's upper vector lanes powered, which power down when unused for a while
  __m256i warm = _mm256_setzero_si256();
  #endif
  #ifdef BENCH_WAIT_GAPS
  // Stalls of the waiting thread over 10000 ticks, counted and summed, printed when the thread ends
  struct WaitGaps
  {
    uint64_t count = 0;
    uint64_t ticks = 0;
    uint64_t waited = 0;
    ~WaitGaps()
    {
      if (waited != 0)
      {
        std::printf("wait gaps over 10000 ticks: %llu, %llu ticks of %llu waited\n", static_cast<unsigned long long>(count),
                    static_cast<unsigned long long>(ticks), static_cast<unsigned long long>(waited));
      }
    }
  };
  thread_local WaitGaps gaps;
  uint64_t previous_tick = read_ticks();
  uint64_t const first_tick = previous_tick;
  #endif
  do
  {
    time_now = std::chrono::steady_clock::now().time_since_epoch();
  #ifdef BENCH_WAIT_GAPS
    uint64_t const tick = read_ticks();
    if (tick - previous_tick > 10000)
    {
      ++gaps.count;
      gaps.ticks += tick - previous_tick;
    }
    previous_tick = tick;
  #endif
  #ifdef BENCH_WARM_VECTOR
    warm = _mm256_add_epi32(warm, _mm256_set1_epi32(static_cast<int>(time_now.count())));
  #endif
  } while (time_now < end_time.time_since_epoch());
  #ifdef BENCH_WAIT_GAPS
  gaps.waited += previous_tick - first_tick;
  #endif
  #ifdef BENCH_WARM_VECTOR
  static volatile int warm_sink;
  warm_sink = _mm256_extract_epi32(warm, 0);
  #endif
#else
  // when in perf use sleep as the other variables add noise
  std::this_thread::sleep_for(max);
#endif
}

inline void set_thread_affinity(size_t cpu_num)
{
#ifdef _WIN32
  if (SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR{1} << cpu_num) == 0)
  {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
  }
#elif defined(__APPLE__)
  (void)cpu_num;
#else
  // Set this thread affinity
  cpu_set_t cpuset;
  CPU_ZERO(&cpuset);
  CPU_SET(cpu_num, &cpuset);

  auto const err = sched_setaffinity(0, sizeof(cpuset), &cpuset);

  if (err == -1)
  {
    throw std::system_error(errno, std::system_category());
  }
#endif
}

inline size_t messages_for_thread(size_t messages_per_iteration, size_t thread_count, size_t thread_num) noexcept
{
  if (thread_count == 0)
  {
    return 0;
  }

  size_t const base_messages_per_thread = messages_per_iteration / thread_count;
  size_t const remainder = messages_per_iteration % thread_count;
  return base_messages_per_thread + (thread_num < remainder ? 1u : 0u);
}

inline void wait_for_all_threads_to_start(std::atomic<size_t>& started_threads, size_t participant_count) noexcept
{
  started_threads.fetch_add(1, std::memory_order_acq_rel);
  while (started_threads.load(std::memory_order_acquire) != participant_count) {}
}

#ifdef BENCH_WITHOUT_PERF
/***/

template <typename TOnThreadStart, typename TLogFunc, typename TOnThreadExit>
inline void run_log_benchmark(size_t num_iterations,
                              size_t messages_per_iteration,
                              TOnThreadStart const& on_thread_start,
                              TLogFunc const& log_func,
                              TOnThreadExit const& on_thread_exit,
                              std::atomic<size_t>& started_threads,
                              size_t participant_count,
                              size_t current_thread_num,
                              std::vector<uint64_t>& latencies,
                              double ns_rdtsc_tick)
{
  // running thread affinity
#ifdef BENCH_PRODUCER_CPU_SHIFT
  set_thread_affinity(current_thread_num + BENCH_PRODUCER_CPU_SHIFT);
#else
  set_thread_affinity(current_thread_num);
#endif

  //  sched_param param{};
  //  param.sched_priority = 99;
  //  if (sched_setscheduler(0, SCHED_FIFO, &param) != 0)
  //    perror("sched_setscheduler");

  on_thread_start();
  wait_for_all_threads_to_start(started_threads, participant_count);

  if (messages_per_iteration == 0)
  {
    on_thread_exit();
    return;
  }

  #ifdef BENCH_INT_INT_DOUBLE
  #elif defined(BENCH_INT_INT_LARGESTR)
  #elif defined(BENCH_VECTOR_LARGESTR)
  // Keep vector contents identical for the same producer in every executable.
  // This matters for loggers that compress or defer formatting the payload.
  std::mt19937 gen{
    static_cast<std::mt19937::result_type>(0xc001'c0deu + current_thread_num)};
  std::uniform_int_distribution<int> length_dist(50, 60); // large strings
  std::uniform_int_distribution<int> char_dist(65, 90);   // ASCII uppercase letters
  #endif

  // Main Benchmark
  for (size_t i = 0; i < num_iterations; ++i)
  {
  #ifdef BENCH_INT_INT_DOUBLE
    // generate a double from i
    double const v = static_cast<double>(i) + (0.1 * static_cast<double>(i));
  #elif defined(BENCH_INT_INT_LARGESTR)
    std::string v{"Lorem ipsum dolor sit amet, consectetur "};
    v += std::to_string(i);
  #elif defined(BENCH_VECTOR_LARGESTR)
    std::vector<std::string> v(16);

    for (std::string& str : v)
    {
      int length = length_dist(gen);
      str.reserve(length);
      for (int char_idx = 0; char_idx < length; ++char_idx)
      {
        str.push_back(static_cast<char>(char_dist(gen)));
      }
    }
  #endif

#ifdef BENCH_PER_MESSAGE
    // Each call timed alone, in ticks, a line per batch in the file BENCH_DUMP_MESSAGES names
    thread_local FILE* message_dump = std::fopen(std::getenv("BENCH_DUMP_MESSAGES"), "a");
    uint64_t ticks[64];
  #ifdef _WIN32
    // The process's page faults during the batch end its line
    PROCESS_MEMORY_COUNTERS faults_before{sizeof(faults_before)};
    GetProcessMemoryInfo(GetCurrentProcess(), &faults_before, sizeof(faults_before));
    ULONG64 cycles_before = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles_before);
  #endif
    auto const start = serialized_rdtsc();
    for (size_t j = 0; j < messages_per_iteration; ++j)
    {
      auto const t0 = serialized_rdtsc();
      log_func(i, j, v);
      ticks[j] = serialized_rdtsc() - t0;
    }
    auto const end = serialized_rdtsc();
    for (size_t j = 0; j < messages_per_iteration; ++j)
    {
      std::fprintf(message_dump, "%llu ", static_cast<unsigned long long>(ticks[j]));
    }
  #ifdef _WIN32
    // Then the cycles charged to the thread over the batch, its calls' ticks and its two timestamps included
    ULONG64 cycles_after = 0;
    QueryThreadCycleTime(GetCurrentThread(), &cycles_after);
    PROCESS_MEMORY_COUNTERS faults_after{sizeof(faults_after)};
    GetProcessMemoryInfo(GetCurrentProcess(), &faults_after, sizeof(faults_after));
    std::fprintf(message_dump, "%lu %llu %llu %llu", faults_after.PageFaultCount - faults_before.PageFaultCount,
                 static_cast<unsigned long long>(cycles_after - cycles_before),
                 static_cast<unsigned long long>(end - start), static_cast<unsigned long long>(start));
  #endif
    std::fprintf(message_dump, "\n");
#else
    auto const start = serialized_rdtsc();
    for (size_t j = 0; j < messages_per_iteration; ++j)
    {
      log_func(i, j, v);
    }
    auto const end = serialized_rdtsc();
#endif

    uint64_t const latency{static_cast<uint64_t>(
      (static_cast<double>(end - start) / static_cast<double>(messages_per_iteration)) * ns_rdtsc_tick)};
    latencies.push_back(latency);

    // send the next log after x time
    wait(MIN_WAIT_DURATION, MAX_WAIT_DURATION, current_thread_num);
  }

  on_thread_exit();
}
#else
/***/
template <typename TOnThreadStart, typename TLogFunc, typename TOnThreadExit>
inline void run_log_benchmark(size_t num_iterations,
                              size_t messages_per_iteration,
                              TOnThreadStart const& on_thread_start,
                              TLogFunc const& log_func,
                              TOnThreadExit const& on_thread_exit,
                              std::atomic<size_t>& started_threads,
                              size_t participant_count,
                              size_t current_thread_num)
{
  // running thread affinity
  set_thread_affinity(current_thread_num);

  on_thread_start();
  wait_for_all_threads_to_start(started_threads, participant_count);

  if (messages_per_iteration == 0)
  {
    on_thread_exit();
    return;
  }

  // Main Benchmark
  for (size_t i = 0; i < num_iterations; ++i)
  {
    // generate a double from i
    double const d = static_cast<double>(i) + (0.1 * static_cast<double>(i));

    for (size_t j = 0; j < messages_per_iteration; ++j)
    {
      log_func(i, j, d);
    }

    // send the next log after x time
    wait(MIN_WAIT_DURATION, MAX_WAIT_DURATION, current_thread_num);
  }

  on_thread_exit();
}
#endif

/***/
template <typename TOnThreadStart, typename TLogFunc, typename TOnThreadExit>
inline void run_benchmark(char const* benchmark_name,
                          int32_t thread_count,
                          size_t num_iterations,
                          size_t messages_per_iteration,
                          TOnThreadStart const& on_thread_start,
                          TLogFunc const& log_func,
                          TOnThreadExit const& on_thread_exit)
{
  if (thread_count <= 0)
  {
#ifdef BENCH_WITHOUT_PERF
    std::cout << "Thread Count 0 - Total messages 0 - " << benchmark_name << "\n | no latency samples |\n\n";
#endif
    return;
  }

  size_t const thread_count_size = static_cast<size_t>(thread_count);

  // main thread affinity
  set_thread_affinity(0);

  double const ns_rdtsc_tick = ns_per_rdtsc_tick();

#ifdef BENCH_WITHOUT_PERF
  // each thread gets a vector of latencies
  std::vector<std::vector<uint64_t>> latencies;
  latencies.resize(thread_count_size);
  for (auto& elem : latencies)
  {
    elem.reserve(num_iterations);
  }
#endif

  std::vector<std::thread> threads;
  threads.reserve(thread_count_size);
  std::atomic<size_t> started_threads{0};
  for (size_t thread_num = 0; thread_num < thread_count_size; ++thread_num)
  {
    size_t const thread_messages_per_iteration =
      messages_for_thread(messages_per_iteration, thread_count_size, thread_num);

#ifdef BENCH_WITHOUT_PERF
    // Spawn num threads
    threads.emplace_back(
      run_log_benchmark<TOnThreadStart, TLogFunc, TOnThreadExit>, num_iterations,
      thread_messages_per_iteration, std::cref(on_thread_start), std::cref(log_func),
      std::cref(on_thread_exit), std::ref(started_threads), thread_count_size, thread_num + 1,
      std::ref(latencies[thread_num]), ns_rdtsc_tick);
#else
    // Spawn num threads
    threads.emplace_back(
      run_log_benchmark<TOnThreadStart, TLogFunc, TOnThreadExit>, num_iterations,
      thread_messages_per_iteration, std::cref(on_thread_start), std::cref(log_func),
      std::cref(on_thread_exit), std::ref(started_threads), thread_count_size, thread_num + 1);
#endif
  }

  // Wait for threads to finish
  for (size_t i = 0; i < thread_count_size; ++i)
  {
    threads[i].join();
  }

#ifdef BENCH_WITHOUT_PERF
  // BENCH_DUMP names a file that gets every batch's latency in order, a line per thread, to see when the slow ones come
  if (char const* dump_path = std::getenv("BENCH_DUMP"))
  {
    if (FILE* dump = std::fopen(dump_path, "a"))
    {
      std::fprintf(dump, "%s threads %d\n", benchmark_name, thread_count);
      for (auto const& elem : latencies)
      {
        for (uint64_t latency : elem)
        {
          std::fprintf(dump, "%llu ", static_cast<unsigned long long>(latency));
        }
        std::fprintf(dump, "\n");
      }
      std::fclose(dump);
    }
  }

  // All threads have finished we can read all latencies
  std::vector<uint64_t> latencies_combined;
  latencies_combined.reserve(num_iterations * thread_count);
  for (auto const& elem : latencies)
  {
    latencies_combined.insert(latencies_combined.end(), elem.begin(), elem.end());
  }

  // Sort all latencies
  std::sort(latencies_combined.begin(), latencies_combined.end());

  size_t const total_messages = num_iterations * messages_per_iteration;
  if (latencies_combined.empty())
  {
    std::cout << "Thread Count " << thread_count << " - Total messages " << total_messages << " - "
              << benchmark_name << "\n | no latency samples |\n\n";
    return;
  }

  size_t const sample_count = latencies_combined.size();
  auto percentile_index = [sample_count](double percentile)
  {
    size_t const nearest_rank =
      static_cast<size_t>(std::ceil(static_cast<double>(sample_count) * percentile));
    return std::min(sample_count - 1, nearest_rank == 0 ? size_t{0} : (nearest_rank - 1));
  };

  std::cout << "Thread Count " << thread_count << " - Total messages " << total_messages << " - "
            << benchmark_name << "\n |  50th | 75th | 90th | 95th | 99th | 99.9th | Worst |\n"
            << " |  " << latencies_combined[percentile_index(0.5)] << "  |  "
            << latencies_combined[percentile_index(0.75)] << "  |  "
            << latencies_combined[percentile_index(0.9)] << "  |  "
            << latencies_combined[percentile_index(0.95)] << "  |  "
            << latencies_combined[percentile_index(0.99)] << "  |  "
            << latencies_combined[percentile_index(0.999)] << "  |  "
            << latencies_combined[latencies_combined.size() - 1] << "  |\n\n";
#endif
}
