#include "call_site_latency_bench.h"
#include "SyncLog.h"
#include "log.h"

#include <stdio.h>
#include <string>

#ifdef _WIN32
  #include <tlhelp32.h>
  #include <timeapi.h>
  #include <psapi.h>
  #pragma comment(lib, "winmm.lib")
  #pragma comment(lib, "psapi.lib")
#endif

// backtest-engine's LOG: its logger thread writes every line to a file
static FILE* gLogFile = nullptr;

#ifdef _WIN32
// A Windows thread gets the process's affinity, not its creator's, so the logger thread is found by its name
static void pinLoggerThread()
{
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  THREADENTRY32 entry{sizeof(entry)};
  for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
  {
    if (entry.th32OwnerProcessID != GetCurrentProcessId())
    {
      continue;
    }
    HANDLE thread = OpenThread(THREAD_SET_INFORMATION | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    PWSTR name = nullptr;
#ifdef BENCH_BTE_NO_HOOK
    // Older bte names no thread: every thread but this one belongs to the logger
    if (thread != nullptr && entry.th32ThreadID != GetCurrentThreadId())
#else
    if (thread != nullptr && SUCCEEDED(GetThreadDescription(thread, &name)) && wcscmp(name, L"AsyncLog") == 0)
#endif
    {
      SetThreadAffinityMask(thread, DWORD_PTR{1} << BENCH_BACKEND_CPU);
    }
    LocalFree(name);
    CloseHandle(thread);
  }
  CloseHandle(snapshot);
}
#endif

#ifndef BENCH_BTE_NO_HOOK
static void writeLine(uint32_t, std::string_view, std::string_view message)
{
  #ifndef BENCH_BTE_NULL_SINK
  fwrite(message.data(), 1, message.size(), gLogFile);
  fputc('\n', gLogFile);
  #else
  static_cast<void>(message);
  #endif
}
#endif

#ifdef BENCH_BTE_ADDRESSES
// Each batch's string and the place in the ring its first record goes to, written to BENCH_DUMP_ADDRESSES at the end
struct BatchStart
{
  uintptr_t text;
  uintptr_t ring;
  size_t limit; // changes only when LOG took the slow path for room
};
static thread_local std::vector<BatchStart> tAddresses;

static void noteAddresses(char const* text)
{
  logdetail::Writer const& writer = *logdetail::tWriter;
  tAddresses.push_back({reinterpret_cast<uintptr_t>(text),
                        reinterpret_cast<uintptr_t>(writer.storage + (writer.position & writer.mask)),
                        writer.limit.load(std::memory_order_relaxed)});
}

static void dumpAddresses()
{
  if (FILE* dump = fopen(getenv("BENCH_DUMP_ADDRESSES"), "a"))
  {
    for (BatchStart const& start : tAddresses)
      fprintf(dump, "%llu %llu %llu ", static_cast<unsigned long long>(start.text),
              static_cast<unsigned long long>(start.ring), static_cast<unsigned long long>(start.limit));
    fprintf(dump, "\n");
    fclose(dump);
  }
  tAddresses.clear();
}
#endif

void bte_benchmark(std::vector<int32_t> thread_count_array, size_t num_iterations_per_thread)
{
#ifndef BENCH_BTE_NO_HOOK
  gLogFile = fopen("bte_call_site_latency.log", "w");
  #ifdef BENCH_BTE_FILE_BUFFER_64K
  setvbuf(gLogFile, nullptr, _IOFBF, 64 * 1024); // as quill's FileSink
  #endif
  setLogLineHook(writeLine);
#endif

  // The logger thread gets the other backends' core; on Linux from the thread whose first LOG starts it
  set_thread_affinity(BENCH_BACKEND_CPU);
  LOG("Logger thread start");
  LOG();
#ifdef _WIN32
  pinLoggerThread();
#endif

#ifdef BENCH_INT_INT_DOUBLE
  auto log_func = [](uint64_t i, uint64_t j, double d)
  { LOG("Logging int: {}, int: {}, double: {}", i, j, d); };
#elif defined(BENCH_INT_INT_LARGESTR) && defined(BENCH_BTE_ADDRESSES)
  auto log_func = [](uint64_t i, uint64_t j, std::string const& s)
  {
    if (j == 0)
      noteAddresses(s.data());
    LOG("Logging int: {}, int: {}, string: {}", i, j, s);
  };
#elif defined(BENCH_INT_INT_LARGESTR)
  auto log_func = [](uint64_t i, uint64_t j, std::string const& s)
  { LOG("Logging int: {}, int: {}, string: {}", i, j, s); };
#endif

  // As quill's: a sleep here lets the producer's core clock down, and its first batches run several times slower
  auto on_start = []()
  {
    LOG("Warm up");
    LOG();
#ifdef BENCH_BTE_PLACES
    logdetail::Writer const& writer = *logdetail::tWriter;
    std::printf("places writer %p storage %p mask %zx published %p\n", static_cast<void const*>(&writer),
                static_cast<void const*>(writer.storage), writer.mask, static_cast<void const*>(writer.published));
#endif
  };

#ifdef BENCH_BTE_ADDRESSES
  auto on_exit = []() { dumpAddresses(); };
#else
  auto on_exit = []() { };
#endif

  for (auto thread_count : thread_count_array)
  {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS before{sizeof(before)};
    GetProcessMemoryInfo(GetCurrentProcess(), &before, sizeof(before));
#endif
    run_benchmark("Logger: bte LOG - Benchmark: Caller Thread Latency", thread_count,
                  num_iterations_per_thread, MESSAGES_PER_ITERATION, on_start, log_func, on_exit);
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS after{sizeof(after)};
    GetProcessMemoryInfo(GetCurrentProcess(), &after, sizeof(after));
    printf("page faults during the run: %lu\n", after.PageFaultCount - before.PageFaultCount);
#endif
  }

  LOG();
#ifndef BENCH_BTE_NO_HOOK
  setLogLineHook(nullptr);
  fclose(gLogFile);
#endif
}

/***/
int main(int, char*[])
{
#if defined(_WIN32) && defined(BENCH_TIMER_1MS)
  timeBeginPeriod(1); // the logger thread's 100 us sleep otherwise lasts until the next 15.6 ms timer tick
#endif
  bte_benchmark(THREAD_LIST_COUNT, ITERATIONS);
}
