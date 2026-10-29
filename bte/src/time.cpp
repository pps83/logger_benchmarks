#include "sysutil.h"
#include "format.h"
#include <boost/int128.hpp>
#include <time.h>
#include <assert.h>
#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#endif
#include <limits>
#include <bit>

typedef boost::int128::uint128 uint128_t;
typedef boost::int128::int128 int128_t;

static constexpr long FREQ_10MHz = 10000000;
static constexpr long FREQ_1GHz = 1000000000;
static constexpr long long FREQ_1THz = 1000000000000LL;
const constexpr unsigned kSecodsPerDay = 24 * 60 * 60; // 86400 seconds per day

#if defined(_WIN32) || defined(__CYGWIN__)
static long long qpcFreq = 0; // will be set by qpcEpochBase
static bool qpcFreq10MHz = false; // qpcFreq == FREQ_10MHz. Will be set by qpcEpochBase
static const bool qpcFreq1GHz = false;
static double qpcFreqInverse = 0.0; // 1.0 / qpcFreq. Will be set by qpcEpochBase
static const long long ftEpochBase = 116444736000000000ULL;
static const long long ftFreq = FREQ_10MHz; // 100-nanosecond intervals, 10MHz

static long long qpf()
{
    LARGE_INTEGER n;
    QueryPerformanceFrequency(&n);
    return n.QuadPart;
}

static long long qpc()
{
    LARGE_INTEGER n;
    QueryPerformanceCounter(&n);
    return n.QuadPart;
}

static long long ft()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    LARGE_INTEGER n;
    n.LowPart = ft.dwLowDateTime;
    n.HighPart = ft.dwHighDateTime;
    return n.QuadPart;
}

static void setFreq()
{
    qpcFreq = qpf();
    qpcFreqInverse = 1.0 / qpcFreq;
    qpcFreq10MHz = qpcFreq == FREQ_10MHz;
}

void debug_SetFreq(long long freq) // used by unit tests
{
    if (freq == 0)
        freq = qpf();
    qpcFreq = freq;
    qpcFreqInverse = 1.0 / qpcFreq;
    qpcFreq10MHz = qpcFreq == FREQ_10MHz;
}

#if defined(__MINGW32__) && !defined(_UCRT)
#define TIME_UTC 1
#define timespec_get timespec_get_mingw
int timespec_get_mingw(struct timespec* ts, int base)
{
    if (base != TIME_UTC || !ts)
        return 0;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    // Convert FILETIME (100-nanosecond intervals since Jan 1, 1601) to Unix Epoch (seconds since Jan 1, 1970)
    ULARGE_INTEGER ull = {.LowPart = ft.dwLowDateTime, .HighPart = ft.dwHighDateTime};
    const long long UNIX_EPOCH_BIAS = 116444736000000000ULL;
    long long unix_ns100 = ull.QuadPart - UNIX_EPOCH_BIAS;
    ts->tv_sec = (time_t)(unix_ns100 / FREQ_10MHz);
    ts->tv_nsec = (long)((unix_ns100 % FREQ_10MHz) * 100);
    return TIME_UTC;
}
#endif
#else
static const bool qpcFreq10MHz = false;
static const bool qpcFreq1GHz = true;
static const long long qpcFreq = FREQ_1GHz;
static const double qpcFreqInverse = 1.0 / qpcFreq;
static const long long ftEpochBase = 0; // timespec_get is already based on UTC
static const long long ftFreq = FREQ_1GHz; // frequency of timespec_get (nanoseconds)

// Not CLOCK_MONOTONIC, which NTP slews: like QueryPerformanceCounter, the clock runs at the rate of the hardware
static long long qpc()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * qpcFreq + ts.tv_nsec;
}

static long long ft()
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return ts.tv_sec * ftFreq + ts.tv_nsec;
}

static void setFreq() // qpcFreq, qpcFreq10MHz, qpcFreqInverse are compile-time const values
{
}
#endif

static long long qpcEpochBase(); // has to be the fist function called (calculates qpc offset related to ft)

long long debug_getQpcFreq()
{
    return qpcFreq;
}

long long debug_qpcEpochBase()
{
    return qpcEpochBase();
}

// the goal of the function is to align values returned by qpc with values returned by ft.
// This way, timeSec will return values very close to values returned by std::time.
// TimeAlignment unit test verifies that this is withing 5 ms.
// TODO: re-anchor at run time at NTP's rate (adjtimex on Linux): the counter's nominal rate is ~15 ppm off real time
static long long NOINLINE calcQpcEpochBase()
{
    setFreq();
    const int N = 3;
    long long tqpc[N], tft[N];
    for (int i = 0; i < N; ++i)
    {
        tqpc[i] = qpc();
        tft[i] = ft();
    }
    auto diff = tft[N - 1] - tft[N - 2];
    if (diff == 1)
        diff++; // if diff=1 then make it 2 (this way tft[N - 2] + diff / 2 would be tft[N - 1])
    long long qpc0_sec, qpc0_nsec;
    if (qpcFreq10MHz)
    {
        qpc0_sec = tqpc[N - 1] / FREQ_10MHz;
        qpc0_nsec = tqpc[N - 1] % FREQ_10MHz;
    }
    else
    {
        qpc0_sec = tqpc[N - 1] / qpcFreq;
        qpc0_nsec = tqpc[N - 1] % qpcFreq;
    }

    auto ftx = tft[N - 2] + diff / 2 - ftEpochBase;

    long long ft0_sec = ftx / ftFreq;
    long long ft0_nsec = ftx % ftFreq;

    return (qpc0_sec - ft0_sec) * qpcFreq + (qpc0_nsec - ft0_nsec * qpcFreq / ftFreq);
}

// Two threads can make the first call: a LOG in a static initializer starts the logger thread
static long long qpcEpochBase()
{
    static const long long qpcBase = calcQpcEpochBase();
    return qpcBase;
}

picosec ticksToPico(long long x)
{
    picosec ret;
    if (qpcFreq10MHz) [[likely]]
        ret = {x / FREQ_10MHz, (x % FREQ_10MHz) * (FREQ_1THz / FREQ_10MHz)};
    else if (qpcFreq1GHz)
        ret = {x / FREQ_1GHz, (x % FREQ_1GHz) * (FREQ_1THz / FREQ_1GHz)};
    else if (qpcFreq < std::numeric_limits<long long>::max() / FREQ_1THz) // check if `(x % qpcFreq) * FREQ_1THz` could overflow long long
        ret = {x / qpcFreq, (x % qpcFreq) * FREQ_1THz / qpcFreq};
    else
        ret = ticksToPicoScaled(x, 1);
    if (ret.ps < 0)
    {
        ret.sec--;
        ret.ps += FREQ_1THz;
    }
    return ret;
}

picosec ticksToPicoScaled(long long snum, unsigned denom)
{
    constexpr uint128_t FREQ_1THz_128 = FREQ_1THz;
    const unsigned shiftR = std::has_single_bit(denom) ? std::countr_zero(denom) : 0;
    const bool negative = snum < 0;
    const unsigned long long num = static_cast<unsigned long long>(negative ? -snum : snum);
    const unsigned long long freq = static_cast<unsigned long long>(qpcFreq);
    unsigned long long ticks = shiftR ? (num >> shiftR) : (num / denom);
    unsigned long long fractionalTicks = shiftR ? (num - (ticks << shiftR)) : (num % denom);
    unsigned long long sec = 0;
    if (ticks >= freq)
    {
        if (qpcFreq10MHz)
        {
            sec = ticks / FREQ_10MHz;
            ticks = ticks % FREQ_10MHz;
        }
        else
        {
            sec = ticks / freq;
            ticks = ticks % freq;
        }
    }
    // auto p = ticks * FREQ_1THz_128 + fractionalTicks * FREQ_1THz_128 / denom;
    auto p = ticks * FREQ_1THz_128 + (shiftR ? (fractionalTicks * FREQ_1THz_128 >> shiftR) : (fractionalTicks * FREQ_1THz_128 / denom));
    auto ps = static_cast<long long>(static_cast<uint64_t>(qpcFreq10MHz ? (p / static_cast<unsigned long>(FREQ_10MHz)) : (p / freq)));
    auto s = static_cast<long long>(sec);
    if (!negative)
        return picosec{s, ps};
    else
        return ps ? picosec{-s - 1, FREQ_1THz - ps} : picosec{-s, 0};
}

picosec nanoToPicoScaled(long long snum, unsigned denom)
{
    constexpr uint128_t FREQ_1THz_128 = FREQ_1THz;
    const unsigned shiftR = std::has_single_bit(denom) ? std::countr_zero(denom) : 0;
    const bool negative = snum < 0;
    const unsigned long long num = static_cast<unsigned long long>(negative ? -snum : snum);
    unsigned long long ticks = shiftR ? (num >> shiftR) : (num / denom);
    unsigned long long fractionalTicks = shiftR ? (num - (ticks << shiftR)) : (num % denom);
    unsigned long long sec = 0;
    if (ticks >= FREQ_1GHz)
    {
        sec = ticks / FREQ_1GHz;
        ticks = ticks % FREQ_1GHz;
    }
    auto p = ticks * FREQ_1THz_128 + (shiftR ? (fractionalTicks * FREQ_1THz_128 >> shiftR) : (fractionalTicks * FREQ_1THz_128 / denom));
    auto ps = static_cast<long long>(static_cast<uint64_t>(p / static_cast<unsigned long>(FREQ_1GHz)));
    auto s = static_cast<long long>(sec);
    if (!negative)
        return picosec{s, ps};
    else
        return ps ? picosec{-s - 1, FREQ_1THz - ps} : picosec{-s, 0};
}

long long ticksToNano(long long x)
{
    if (qpcFreq10MHz)
        return x * 100;
    if (qpcFreq1GHz)
        return x;
    long long x_sec = x / qpcFreq;
    long long x_rem = x % qpcFreq;
    return FREQ_1GHz * x_sec + FREQ_1GHz * x_rem / qpcFreq;
}

long long ticksToMicro(long long x)
{
    if (qpcFreq10MHz)
        return x / 10;
    if (qpcFreq1GHz)
        return x / 1000;
    long long x_sec = x / qpcFreq;
    long long x_rem = x % qpcFreq;
    return 1000000LL * x_sec + 1000000LL * x_rem / qpcFreq;
}

long long ticksToMs(long long x)
{
    if (qpcFreq10MHz)
        return x / 10000;
    if (qpcFreq1GHz)
        return x / 1000000;
    long long x_sec = x / qpcFreq;
    long long x_rem = x % qpcFreq;
    return 1000LL * x_sec + 1000LL * x_rem / qpcFreq;
}

long long ticksToSec(long long x)
{
    if (qpcFreq10MHz)
        return x / FREQ_10MHz;
    if (qpcFreq1GHz)
        return x / FREQ_1GHz;
    return x / qpcFreq;
}

double ticksToTime(long long x)
{
    return x * qpcFreqInverse;
}

long long timeTicks()
{
    return qpc() - qpcEpochBase();
}

long long timeTicksRaw()
{
    assert(qpcFreq != 0); // check that qpcEpochBase() was already called
    return qpc();
}

#if !defined(_WIN32) && !defined(__CYGWIN__)
long long HrTimer::timeTicks()
{
    assert(qpcFreq != 0); // check that qpcEpochBase() was already called
    return qpc();
}
#endif

long long timeMicro()
{
    return ticksToMicro(timeTicks());
}

long long timeMs()
{
    return ticksToMs(timeTicks());
}

long long timeSec()
{
    return ticksToSec(timeTicks());
}

double time()
{
    return ticksToTime(timeTicks());
}

std::string formatTicksImpl(long long ticks_signed, unsigned long long freq)
{
    assert(ticks_signed > INT64_MIN);
    const char* sign = ticks_signed < 0 ? "-" : "";
    auto ticks = static_cast<unsigned long long>(ticks_signed < 0 ? -ticks_signed : ticks_signed);
    if (ticks >= freq) // >= 1sec, print as seconds (or minutes/hours/days)
    {
        auto days = (ticks / freq) / kSecodsPerDay;
        auto sec = static_cast<unsigned>((ticks / freq) % kSecodsPerDay);
        auto ms = static_cast<unsigned>(1000 * (ticks % freq) / freq);
        assert(ms >= 0 && ms < 1000);
        if (days >= 10) // more than 10 days: print days, hours, minutes
            return fmtStr("%s%llud %02u:%02u", sign, days, sec / 3600, sec % 3600 / 60);
        else if (days) // print days
            return fmtStr("%s%llud %02u:%02u:%02u", sign, days, sec / 3600, sec % 3600 / 60, sec % 60);
        else if (sec >= 3600) // print hours
            return fmtStr("%s%u:%02u:%02u", sign, sec / 3600, sec % 3600 / 60, sec % 60);
        else if (sec >= 60) // print minutes
            return fmtStr("%s%02u:%02u.%usec", sign, sec / 60, sec % 60, ms / 100);
        else if (sec > 10)
            return fmtStr("%s%u.%usec", sign, sec, ms / 100);
        else if (sec > 6)
            return fmtStr("%s%u.%02usec", sign, sec, ms / 10);
        else
            return fmtStr("%s%u.%03usec", sign, sec, ms);
    }
    ticks *= 1000;
    if (ticks * 15 > freq * 10) // >= 667us, print as milliseconds
    {
        auto ms = static_cast<unsigned>(ticks / freq);
        auto us = static_cast<unsigned>(1000 * (ticks % freq) / freq);
        assert(us >= 0 && us < 1000);
        if (ms > 100)
            return fmtStr("%s%u.%ums", sign, ms, us / 100);
        else if (ms > 10)
            return fmtStr("%s%u.%02ums", sign, ms, us / 10);
        else
            return fmtStr("%s%u.%03ums", sign, ms, us);
    }
    ticks *= 1000;
    if (ticks * 15 > freq * 10) // >= 667ns, print as microseconds
    {
        auto us = static_cast<unsigned>(ticks / freq);
        auto ns = static_cast<unsigned>(1000 * (ticks % freq) / freq);
        assert(ns >= 0 && ns < 1000);
        if (us > 100)
            return fmtStr("%s%u.%uus", sign, us, ns / 100);
        else if (us > 10)
            return fmtStr("%s%u.%02uus", sign, us, ns / 10);
        else
            return fmtStr("%s%u.%03uus", sign, us, ns);
    }
    ticks *= 1000;
    auto ns = static_cast<unsigned>(ticks / freq);
    auto ps = static_cast<unsigned>(1000 * (ticks % freq) / freq);
    assert(ps >= 0 && ps < 1000);
    if (ns > 100)
        return fmtStr("%s%u.%uns", sign, ns, ps / 100);
    else if (ns > 10)
        return fmtStr("%s%u.%02uns", sign, ns, ps / 10);
    else if (ns > 0 || ps > 0)
        return fmtStr("%s%u.%03uns", sign, ns, ps);
    else
        return "0ns";
}

std::string formatTicks(long long ticks_signed)
{
    return formatTicksImpl(ticks_signed, static_cast<unsigned long long>(qpcFreq));
}

std::string formatTicks(picosec time)
{
    assert(time.ps >= 0 && time.ps < FREQ_1THz);
    const char* sign = time.sec < 0 ? "-" : "";
    if (time.sec < 0)
    {
        time.sec = std::abs(time.sec + 1);
        if (time.ps)
            time.ps = FREQ_1THz - time.ps;
    }
    if (time.sec) // >= 1sec, print as seconds (or minutes/hours/days)
    {
        auto days = static_cast<unsigned long long>(time.sec / kSecodsPerDay);
        auto sec = static_cast<unsigned>(time.sec % kSecodsPerDay);
        auto ms = static_cast<unsigned>(time.ps / (FREQ_1THz / 1000));
        if (!(ms >= 0 && ms < 1000))
        {
            LLOG("error: invalid value for ps:%lld", time.ps);
        }
        assert(ms >= 0 && ms < 1000);
        if (days >= 10) // more than 10 days: print days, hours, minutes
            return fmtStr("%s%llud %02u:%02u", sign, days, sec / 3600, sec % 3600 / 60);
        else if (days) // print days
            return fmtStr("%s%llud %02u:%02u:%02u", sign, days, sec / 3600, sec % 3600 / 60, sec % 60);
        else if (sec >= 3600) // print hours
            return fmtStr("%s%u:%02u:%02u", sign, sec / 3600, sec % 3600 / 60, sec % 60);
        else if (sec >= 60) // print minutes
            return fmtStr("%s%02u:%02u.%usec", sign, sec / 60, sec % 60, ms / 100);
        else if (sec > 10)
            return fmtStr("%s%u.%usec", sign, sec, ms / 100);
        else if (sec > 6)
            return fmtStr("%s%u.%02usec", sign, sec, ms / 10);
        else
            return fmtStr("%s%u.%03usec", sign, sec, ms);
    }
    auto ticks = time.ps * 1000ull;
    if (ticks * 15 > FREQ_1THz * 10) // >= 667us, print as milliseconds
    {
        auto ms = static_cast<unsigned>(ticks / FREQ_1THz);
        auto us = static_cast<unsigned>((ticks % FREQ_1THz) / (FREQ_1THz / 1000));
        assert(us >= 0 && us < 1000);
        if (ms > 100)
            return fmtStr("%s%u.%ums", sign, ms, us / 100);
        else if (ms > 10)
            return fmtStr("%s%u.%02ums", sign, ms, us / 10);
        else
            return fmtStr("%s%u.%03ums", sign, ms, us);
    }
    ticks *= 1000;
    if (ticks * 15 > FREQ_1THz * 10) // >= 667ns, print as microseconds
    {
        auto us = static_cast<unsigned>(ticks / FREQ_1THz);
        auto ns = static_cast<unsigned>((ticks % FREQ_1THz) / (FREQ_1THz / 1000));
        assert(us >= 0 && us < 1000);
        if (us > 100)
            return fmtStr("%s%u.%uus", sign, us, ns / 100);
        else if (us > 10)
            return fmtStr("%s%u.%02uus", sign, us, ns / 10);
        else
            return fmtStr("%s%u.%03uus", sign, us, ns);
    }
    ticks *= 1000;
    auto ns = static_cast<unsigned>(ticks / FREQ_1THz);
    auto ps = static_cast<unsigned>(time.ps % 1000);
    assert(ps == unsigned((ticks % FREQ_1THz) / (FREQ_1THz / 1000)));
    assert(ps >= 0 && ps < 1000);
    if (ns > 100)
        return fmtStr("%s%u.%uns", sign, ns, ps / 100);
    else if (ns > 10)
        return fmtStr("%s%u.%02uns", sign, ns, ps / 10);
    else if (ns > 0 || ps > 0)
        return fmtStr("%s%u.%03uns", sign, ns, ps);
    else
        return "0ns";
}

namespace tsc
{
long long qpcToTsc(long long t);
long long debug_getQpcFreq();
}

namespace
{
struct StartTime
{
    int64_t ticks, tsc_ticks;
    double time, tsc_time;

    void watchdog() const // watchdog timer to verity tsc vs qpc timer correctness
    {
        auto ticksDiff = timeTicks() - ticks;
        auto tsc_ticksDiff = tsc::timeTicks() - tsc_ticks;
        if (ticksDiff > 10 * qpcFreq) // check measurement if process run over 10 seconds
        {
            auto measuredTscFreq = static_cast<long long>(int128_t(qpcFreq) * tsc_ticksDiff / ticksDiff);
            auto tscFreq = tsc::debug_getQpcFreq();
            auto err = std::abs((measuredTscFreq - tscFreq) * 100.0 / tscFreq);
            if (err > 0.0001) // clock drift of 1/1M is roughly is 30 sec or more per year
            {
                // Note: formatTicks cannot be used here, as StartTime dtor runs from compiler segment on windows
                printf("tscFreq:%lld, measuredTscFreq:%lld (measurement err:%f%%), time elapsed:%lld.%03lld seconds\n",
                    tscFreq, measuredTscFreq, err, ticksToSec(ticksDiff), ticksToMs(ticksDiff) % 1000);
            }
        }
    }

    ~StartTime()
    {
        watchdog();
    }
};

static StartTime getTimeNow()
{
    auto t = timeTicks();
    tsc::timeTicks(); // trigger tsc::qpcEpochBase / initTsc()
    auto t_tsc = tsc::qpcToTsc(t);
    return StartTime{t, t_tsc, ticksToTime(t), tsc::ticksToTime(t_tsc)};
}
}

#ifdef _MSC_VER
#pragma warning (disable:4074) // initializers put in compiler initialization area
#pragma init_seg(compiler)
#endif
static const StartTime st = getTimeNow(); // global var

int64_t getStartTimeTicks()
{
    assert(st.ticks);
    return st.ticks;
}

double getStartTime()
{
    assert(st.time);
    return st.time;
}

namespace tsc
{
int64_t getStartTimeTicks()
{
    assert(st.tsc_ticks);
    return st.tsc_ticks;
}

double getStartTime()
{
    assert(st.tsc_time);
    return st.tsc_time;
}
}
