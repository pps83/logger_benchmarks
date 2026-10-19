// The logger thread's rate on a LOG burst far bigger than the ring, against the same lines written synchronously
#include "../call_site_latency/call_site_latency_bench_common.h"
#include "SyncLog.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

static const char* stdoutKind()
{
#ifdef _WIN32
  HANDLE const out = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD mode;
  switch (GetFileType(out))
  {
  case FILE_TYPE_CHAR:
    return GetConsoleMode(out, &mode) ? "console" : "NUL";
  case FILE_TYPE_PIPE:
    return "pipe";
  case FILE_TYPE_DISK:
    return "file";
  default:
    return "unknown";
  }
#else
  return "unknown";
#endif
}

static void logLines(int lines)
{
  for (int i = 0; i < lines; ++i)
  {
    LOG("numbers {} {} {}", i, i * 7, i * 0.5);
  }
  LOG();
}

static void writeLines(int lines)
{
  std::string line;
  char text[64];
  for (int i = 0; i < lines; ++i)
  {
    int const length = snprintf(text, sizeof(text), "numbers %d %d %g", i, i * 7, i * 0.5);
    line.assign(kLogPrefixSize, ' ');
    line.append(text, static_cast<size_t>(length));
    line += '\n';
    writeLogLine(line, ticksToTime(timeTicks()), 1234, "main", false);
  }
  flushSyncLog();
}

static double usPerLine(void (*run)(int), int lines, double nsPerTick)
{
  uint64_t const start = serialized_rdtsc();
  run(lines);
  return static_cast<double>(serialized_rdtsc() - start) * nsPerTick / lines / 1000;
}

int main(int argc, char** argv)
{
  int const lines = argc > 1 ? atoi(argv[1]) : 200000;
  double const nsPerTick = ns_per_rdtsc_tick();
  double const sync = usPerLine(writeLines, lines, nsPerTick);
  LOG("start");
  LOG();
  double const async = usPerLine(logLines, lines, nsPerTick);
  AsyncLogSilent = true;
  double const silent = usPerLine(logLines, lines, nsPerTick);
  AsyncLogSilent = false;
  FILE* results = argc > 2 ? fopen(argv[2], "ab") : stderr;
  fprintf(results, "stdout %s, %d lines: synchronous %.2f us, logger thread %.2f us, logger thread without output %.2f us\n",
          stdoutKind(), lines, sync, async, silent);
  if (results != stderr)
    fclose(results);
  return 0;
}
