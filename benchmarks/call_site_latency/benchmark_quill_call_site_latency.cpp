#include "call_site_latency_bench.h"
#include "benchmark_quill_frontend_options.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"
#include "quill/std/Vector.h"

#include <stdio.h>
#include <string>

#ifdef _WIN32
  #include <psapi.h>
  #pragma comment(lib, "psapi.lib")
#endif

using frontend_t = quill::FrontendImpl<CustomFrontendOptions>;
using logger_t = quill::LoggerImpl<CustomFrontendOptions>;

#ifdef BENCH_MORPH_HOOKS
static void morph_poll_begin() {}
#endif

void quill_benchmark(std::vector<int32_t> thread_count_array, size_t num_iterations_per_thread)
{
  std::remove("benchmark_quill_unbounded_call_site_latency.log");

  // Setup
  quill::BackendOptions backend_options;
  backend_options.cpu_affinity = {BENCH_BACKEND_CPU};
#ifndef BENCH_QUILL_BACKEND_SLEEPS
  backend_options.sleep_duration = std::chrono::nanoseconds{0};
#endif
  // bte's logger options, one step at a time (our fork only)
#ifdef BENCH_MORPH_RAW_TSC
  backend_options.raw_rdtsc_timestamps = true;
#endif
#ifdef BENCH_MORPH_HOOKS
  backend_options.check_printable_char = {};
  backend_options.backend_worker_on_poll_begin = morph_poll_begin;
#endif
  quill::Backend::start(backend_options);

  // wait for the backend thread to start
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Frontend
  auto file_sink = frontend_t::create_or_get_sink<quill::FileSink>(
    "benchmark_quill_unbounded_call_site_latency.log",
    []()
    {
      quill::FileSinkConfig cfg;
      cfg.set_open_mode('w');
      cfg.set_filename_append_option(quill::FilenameAppendOption::None);
      return cfg;
    }(),
    quill::FileEventNotifier{});

  logger_t* logger =
    frontend_t::create_or_get_logger("root", std::move(file_sink),
    quill::PatternFormatterOptions{"%(time) [%(thread_id)] %(short_source_location:<28) "
                                   "LOG_%(log_level:<9) %(logger:<12) %(message)",
                                   "%H:%M:%S.%Qns", quill::Timezone::GmtTime, false});

  // Define a logging lambda
#ifdef BENCH_INT_INT_DOUBLE
  char const* benchmark_type = "[benchmark_type: int_int_double]";

  auto log_func = [logger](uint64_t i, uint64_t j, double d)
  { LOG_INFO(logger, "Logging int: {}, int: {}, double: {}", i, j, d); };
#elif defined(BENCH_INT_INT_LARGESTR)
  char const* benchmark_type = "[benchmark_type: int_int_largestr]";

  auto log_func = [logger](uint64_t i, uint64_t j, std::string const& s)
  { LOG_INFO(logger, "Logging int: {}, int: {}, string: {}", i, j, s); };
#elif defined(BENCH_VECTOR_LARGESTR)
  char const* benchmark_type = "[benchmark_type: vector_largestr]";

  auto log_func = [logger](uint64_t i, uint64_t j, std::vector<std::string> const& s)
  { LOG_INFO(logger, "Logging int: {}, int: {}, vector: {}", i, j, s); };
#else
  static_assert(false,
                "define BENCH_INT_INT_DOUBLE, BENCH_INT_INT_LARGESTR, or BENCH_VECTOR_LARGESTR");
#endif

  auto on_start = [logger]()
  {
    LOG_INFO(logger, "Warm up");
    logger->flush_log(0);
  };

  auto on_exit = []() { frontend_t::get_all_loggers().front()->flush_log(); };

  // Run the benchmark for n threads
  std::string benchmark_name = "Logger: Quill - Benchmark: Caller Thread Latency, Unbounded";
  benchmark_name += " - ";
  benchmark_name += benchmark_type;

  for (auto thread_count : thread_count_array)
  {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS before{sizeof(before)};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
#endif
    run_benchmark(benchmark_name.c_str(), thread_count, num_iterations_per_thread,
                  MESSAGES_PER_ITERATION, on_start, log_func, on_exit);
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS after{sizeof(after)};
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    printf("page faults during the run: %lu\n", after.PageFaultCount - before.PageFaultCount);
#endif
  }
}

/***/
int main(int argc, char* argv[]) { quill_benchmark(THREAD_LIST_COUNT, ITERATIONS); }
