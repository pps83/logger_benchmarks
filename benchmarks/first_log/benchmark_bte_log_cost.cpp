// A line's cost on the calling thread, per kind of line and per way of logging it, and the pieces of a synchronous write
#include "../call_site_latency/call_site_latency_bench_common.h"
#include "SyncLog.h"
#include "log.h"
#include "util.h"

#include <algorithm>
#include <format>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

struct Point
{
  int x;
  int y;
};

template <>
struct std::formatter<Point> : std::formatter<int>
{
  auto format(Point const& point, std::format_context& context) const
  {
    return std::format_to(context.out(), "({}, {})", point.x, point.y);
  }
};

static std::string const gText(80, 'x');
static std::string gFormatted;
static std::string gLine;
static FILE* gPieceFile = nullptr;

// Two literals in turn, so no LOG repeats the one before it
static char const* literalText(int i)
{
  return i % 2 == 0 ? "connection lost" : "connection restored";
}

static void logLiteral(int i)
{
  if (i % 2 == 0)
  {
    LOG("connection lost");
  }
  else
  {
    LOG("connection restored");
  }
}

static void logNumbers(int i)
{
  LOG("numbers {} {} {}", i, i * 7, i * 0.5);
}

static void logString(int i)
{
  LOG("string {} {}", i, gText);
}

static void logCustom(int i)
{
  LOG("custom {} {}", i, Point {i, -i});
}

static void logPrintfNumbers(int i)
{
  LOG("numbers %d %d %f", i, i * 7, i * 0.5);
}

static void logPrintfString(int i)
{
  LOG("string %d %s", i, gText.c_str());
}

static char gBuffer[256];

static void formatPrintfNumbers(int i)
{
  snprintf(gBuffer, sizeof(gBuffer), "numbers %d %d %f", i, i * 7, i * 0.5);
  gFormatted = gBuffer;
}

static void formatPrintfString(int i)
{
  snprintf(gBuffer, sizeof(gBuffer), "string %d %s", i, gText.c_str());
  gFormatted = gBuffer;
}

static void formatLiteral(int i)
{
  gFormatted = literalText(i);
}

static void formatNumbers(int i)
{
  gFormatted = std::format("numbers {} {} {}", i, i * 7, i * 0.5);
}

static void formatString(int i)
{
  gFormatted = std::format("string {} {}", i, gText);
}

static void formatCustom(int i)
{
  gFormatted = std::format("custom {} {}", i, Point {i, -i});
}

using Format = void (*)(int);

// Formats the line on the calling thread and queues the finished text, as a LOG with a custom type does
template <Format F>
static void logFormatted(int i)
{
  F(i);
  LOG(std::string_view(gFormatted));
}

template <Format F, bool Flush>
static void writeSync(int i)
{
  F(i);
  gLine.assign(kLogPrefixSize, ' ');
  gLine += gFormatted;
  gLine += '\n';
  writeLogLine(gLine, ticksToTime(timeTicks()), 1234, "main", Flush);
}

static void fileWrite(int)
{
  fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, gPieceFile);
}

static void fileWriteFlush(int)
{
  fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, gPieceFile);
  fflush(gPieceFile);
}

static void writeStdout(int)
{
  fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, stdout);
}

static void writeDebugger(int)
{
#ifdef _WIN32
  OutputDebugStringA(gLine.c_str() + kLogPrefixSize);
#endif
}

static void noFlush()
{
}

static void flushLog()
{
  LOG();
}

// Median over batches of ns per call, with the flush run between batches and left out of the time
static double medianNs(void (*call)(int), void (*flush)(), int batches, int perBatch, double nsPerTick)
{
  std::vector<double> results;
  for (int batch = 0; batch < batches; ++batch)
  {
    uint64_t const start = serialized_rdtsc();
    for (int i = 0; i < perBatch; ++i)
    {
      call(i);
    }
    results.push_back(static_cast<double>(serialized_rdtsc() - start) * nsPerTick / perBatch);
    flush();
  }
  std::sort(results.begin(), results.end());
  return results[results.size() / 2];
}

struct Kind
{
  char const* name;
  Format format;
  void (*deferred)(int);
  void (*atCall)(int);
  void (*syncNoFlush)(int);
  void (*syncFlush)(int);
};

int main()
{
  double const nsPerTick = ns_per_rdtsc_tick();
  Kind const kinds[] = {
    {"literal", formatLiteral, logLiteral, logFormatted<formatLiteral>, writeSync<formatLiteral, false>,
     writeSync<formatLiteral, true>},
    {"three numbers", formatNumbers, logNumbers, logFormatted<formatNumbers>, writeSync<formatNumbers, false>,
     writeSync<formatNumbers, true>},
    {"number and 80-char string", formatString, logString, logFormatted<formatString>, writeSync<formatString, false>,
     writeSync<formatString, true>},
    {"number and custom type", formatCustom, nullptr, logCustom, writeSync<formatCustom, false>,
     writeSync<formatCustom, true>},
    {"printf three numbers", formatPrintfNumbers, logPrintfNumbers, logFormatted<formatPrintfNumbers>,
     writeSync<formatPrintfNumbers, false>, writeSync<formatPrintfNumbers, true>},
    {"printf number and 80-char string", formatPrintfString, logPrintfString, logFormatted<formatPrintfString>,
     writeSync<formatPrintfString, false>, writeSync<formatPrintfString, true>},
  };
  constexpr int kKinds = 6;

  double format[kKinds];
  double noFlushNs[kKinds];
  double flushNs[kKinds];
  for (int k = 0; k < kKinds; ++k)
  {
    format[k] = medianNs(kinds[k].format, noFlush, 50, 1000, nsPerTick);
    noFlushNs[k] = medianNs(kinds[k].syncNoFlush, noFlush, 50, 100, nsPerTick);
    flushNs[k] = medianNs(kinds[k].syncFlush, noFlush, 50, 100, nsPerTick);
  }

  gPieceFile = fopen("pieces.txt", "wb");
  writeSync<formatString, false>(0);
  double const fileNs = medianNs(fileWrite, noFlush, 50, 100, nsPerTick);
  double const fileFlushNs = medianNs(fileWriteFlush, noFlush, 50, 100, nsPerTick);
  double const stdoutNs = medianNs(writeStdout, noFlush, 50, 100, nsPerTick);
  double const debuggerNs = medianNs(writeDebugger, noFlush, 50, 100, nsPerTick);
  fclose(gPieceFile);

  LOG("start");
  LOG();
  double deferred[kKinds];
  double atCall[kKinds];
  for (int k = 0; k < kKinds; ++k)
  {
    deferred[k] = kinds[k].deferred != nullptr ? medianNs(kinds[k].deferred, flushLog, 200, 1000, nsPerTick) : -1;
    atCall[k] = medianNs(kinds[k].atCall, flushLog, 200, 1000, nsPerTick);
  }

  for (int k = 0; k < kKinds; ++k)
  {
    fprintf(stderr, "%s: LOG deferred %.1f ns, LOG formatted at the call %.1f ns, format alone %.0f ns, "
            "sync without flush %.0f ns, sync with flush %.0f ns\n", kinds[k].name, deferred[k], atCall[k], format[k],
            noFlushNs[k], flushNs[k]);
  }
  fprintf(stderr, "pieces: file write %.0f ns, file write and flush %.0f ns, stdout %.0f ns, debugger %.0f ns\n", fileNs,
          fileFlushNs, stdoutNs, debuggerNs);
  return 0;
}
