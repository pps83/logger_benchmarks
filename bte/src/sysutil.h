#pragma once
// Time ticks, process and thread identity: what the logger needs from the platform, with nothing of the engine's data
#include <assert.h>
#include <stdint.h>
#include <string>

#if (defined(_WIN32) || defined(__CYGWIN__)) && (defined(_M_IX86) || defined(__i386__))
extern "C" __declspec(dllimport) int __stdcall QueryPerformanceCounter(union _LARGE_INTEGER*);
#elif defined(_WIN32) || defined(__CYGWIN__)
extern "C" __declspec(dllimport) int           QueryPerformanceCounter(union _LARGE_INTEGER*);
#endif

#define UNUSED(x) (void)x

long long timeTicksRaw(); // not relative to UTC epoch

// time, timeSec, timeMs, timeMicro, timeTicks return monotonic steady time from 1970 aligned with wall clock. Alignment
// is calculated once at process start. If wall clock is shifted while process runs, then these will not match wall clock.
long long timeTicks();
long long timeMicro();
long long timeMs();
long long timeSec();
double time();

// similar to timeTicks, but returns nanoseconds.
long long timeNanoStart();
long long timeNanoStop();

// similar to timespec, except ps is picosecond resolution. Valid values for ps: [0, 999999999999]
struct picosec { long long sec, ps; };

picosec ticksToPico(long long ticks);
picosec ticksToPicoScaled(long long ticks, unsigned scale); // equivalent to ticksToPico(ticks / scale) without losing precision
picosec nanoToPicoScaled(long long ticks, unsigned scale);
long long ticksToNano(long long ticks);
long long ticksToMicro(long long ticks);
long long ticksToMs(long long ticks);
long long ticksToSec(long long ticks);
double ticksToTime(long long ticks); // seconds with fractional part

std::string formatTicks(long long ticks); // format ticks as either ns, us, ms, or seconds depending on the range, suitable for logging/output
std::string formatTicks(picosec time); // format time as either ns, us, ms, or seconds depending on the range, suitable for logging/output

int64_t getStartTimeTicks(); // get value of timeTicks() at process start
double getStartTime(); // get value of time() at process start, equivalent to ticksToTime(getStartTimeTicks())

namespace tsc
{
long long timeTicksRaw(); // not relative to UTC epoch
long long rawToTicks(long long raw); // timeTicksRaw() value to timeTicks()

// time, timeSec, timeMs, timeMicro, timeTicks return monotonic steady time from 1970 aligned with wall clock. Alignment
// is calculated once at process start. If wall clock is shifted while process runs, then these will not match wall clock.
long long timeTicks();
long long timeMicro();
long long timeMs();
long long timeSec();
double time();

long long ticksToNano(long long x);
long long ticksToMicro(long long x);
long long ticksToMs(long long x);
long long ticksToSec(long long x);
double ticksToTime(long long x);

std::string formatTicks(long long ticks);
picosec ticksToPico(long long x);
picosec ticksToPicoScaled(long long snum, unsigned denom);

int64_t getStartTimeTicks(); // get value of timeTicks() at process start
double getStartTime(); // get value of time() at process start, equivalent to ticksToTime(getStartTimeTicks())
}

class HrTimer
{
    long long elapsed, t0;

public:
    HrTimer()
        : elapsed(-1)
        , t0(timeTicks())
    {
    }

    void start()
    {
        elapsed = -1;
        t0 = timeTicks();
    }

    void stop()
    {
        assert(t0 != -1);
        if (t0 != -1)
        {
            elapsed = timeTicks() - t0;
            t0 = -1;
        }
    }

    long long elapsedTicks() const
    {
        return elapsed != -1 ? elapsed : timeTicks() - t0;
    }

    long long elapsedNano() const
    {
        return ticksToNano(elapsedTicks());
    }

    long long elapsedMicro() const
    {
        return ticksToMicro(elapsedTicks());
    }

    long long elapsedMs() const
    {
        return ticksToMs(elapsedTicks());
    }

    long long elapsedSec() const
    {
        return ticksToSec(elapsedTicks());
    }

    double elapsedTime() const
    {
        return ticksToTime(elapsedTicks());
    }

    std::string elapsedTimeString() const
    {
        return formatTicks(elapsedTicks());
    }

private:
#if defined(_WIN32) || defined(__CYGWIN__)
    static inline long long timeTicks()
    {
        long long q;
        QueryPerformanceCounter(reinterpret_cast<union _LARGE_INTEGER*>(&q));
        return q;
    }
#else
    static long long timeTicks(); // similar to ::timeTicks, except doesn't do epoch alignment for the timestamp
#endif
};

// process or thread related utils
unsigned currentProcessId();
unsigned currentThreadId();
void setThreadName(const char* threadName);
const char* givenThreadName(); // what setThreadName last gave the calling thread, "" if nothing
void setThreadNameListener(void (*listener)(const char* threadName));
bool debuggerPresent();

#ifdef _WIN32
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
#endif
