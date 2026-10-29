#include "sysutil.h"
#include "format.h"
#include "GetProcAddr.h"
#include "libdivide.h"
#include <boost/int128.hpp>
#include <time.h>
#include <assert.h>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#ifdef _MSC_VER
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
#if defined(__clang__) || defined(__GNUC__)
#include <cpuid.h>
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
#define WINDOWS_TSC
#endif
#endif
#include <chrono>
#include <limits>
#include <bit>
#include <thread>
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#define LINUX_TSC
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

typedef boost::int128::uint128 uint128_t;
typedef boost::int128::int128 int128_t;

long long debug_qpcEpochBase();
long long debug_getQpcFreq();
std::string formatTicksImpl(long long ticks_signed, unsigned long long qpcFreq);

namespace tsc
{
static constexpr long FREQ_1KHz = 1000;
static constexpr long FREQ_1MHz = 1000000;
static constexpr long FREQ_1GHz = 1000000000;
static constexpr long long FREQ_1THz = 1000000000000LL;

#if defined(WINDOWS_TSC)
static constexpr long FREQ_10MHz = 10000000;

static volatile uint64_t* RtlpHypervisorSharedUserVa_tscScale = nullptr;
static volatile int64_t* RtlpHypervisorSharedUserVa_tscBias = nullptr;
static volatile uint8_t* SharedUserData_qpcShift = nullptr;
static volatile int64_t* SharedUserData_qpcBias = nullptr;

static int64_t qpcBias = 0;
static uint64_t tscScale = 0;
#endif

static long long qpcFreq = 0; // will be set by qpcEpochBase
static double qpcFreqInverse = 0.0; // 1.0 / qpcFreq. Will be set by qpcEpochBase

static libdivide::libdivide_u64_branchfree_t qpcFreqDiv;
static unsigned long long maxNsInU64;
static unsigned long long maxUsInU64;
static unsigned long long maxMsInU64;

union CpuidInfo
{
    uint32_t reg[4];
};

#if defined(WINDOWS_TSC) || defined(LINUX_TSC)
#if defined(_MSC_VER) && !defined(__clang__)
static CpuidInfo cpuid(uint32_t function_id)
{
    CpuidInfo ret = {};
    __cpuid((int*)ret.reg, function_id);
    return ret;
}
#else
static CpuidInfo cpuid(uint32_t function_id)
{
    CpuidInfo ret = {};
    __get_cpuid(function_id, &ret.reg[0], &ret.reg[1], &ret.reg[2], &ret.reg[3]);
    return ret;
}
#endif
static long long qpcEpochBase(); // has to be the fist function called (calculates tsc offset related to ft)

static bool supports_constant_tsc()
{
    CpuidInfo info = {};
    if (static_cast<unsigned>(cpuid(0x80000000).reg[0]) >= 0x80000007)
        info = cpuid(0x80000007);
    return !!(info.reg[3] & (1u << 8));
}

static uint64_t tsc_frequency_from_cpuid()
{
    //see https://en.wikipedia.org/wiki/CPUID#EAX=15h_and_EAX=16h:_CPU,_TSC,_Bus_and_Core_Crystal_Clock_Frequencies
    auto cpuid0_eax = cpuid(0).reg[0];
    if (cpuid0_eax >= 0x15)
    {
        const CpuidInfo info = cpuid(0x15);
        if (info.reg[0] && info.reg[1] && info.reg[2])
            return (uint64_t)info.reg[2] * info.reg[1] / info.reg[0];
#if defined(WINDOWS_TSC)
        if (cpuid0_eax >= 0x16)
            return cpuid(0x16).reg[0] * 1000000; // value might be off by as much as 1%, so Linux measures it instead
#endif
    }
    return 0;
}

#endif
#if defined(WINDOWS_TSC)
static void initTscWin10()
{
    volatile uint8_t* SharedUserData = (uint8_t*)0x7ffe0000;
    volatile uint64_t* RtlpHypervisorSharedUserVa = nullptr;

    const uint32_t kernel_major = *(uint32_t*)(SharedUserData + 0x026c);
    if (kernel_major != 10)
        throw_runtime_error("Error: wrong kernel major: %u", kernel_major);
    const uint32_t win_10_build_number = *(uint32_t*)(SharedUserData + 0x0260);
    if (win_10_build_number <= 14393)
        throw_runtime_error("Error: wrong build number: %u", win_10_build_number);
    const uint8_t flags = *(SharedUserData + 0x3c6);
    const uint8_t bypass_syscall = flags & 0x1;
    const uint8_t use_hypervisor_page = flags & 0x2;
    const uint8_t use_rdtscp = flags & 0x80;
    if (!bypass_syscall || !use_hypervisor_page || !use_rdtscp)
        throw_runtime_error("Error: wrong tsc flags");

    LONG(NTAPI * NtQuerySystemInformation)(ULONG, PVOID, ULONG, PULONG);
    if (!GetProcAddr("ntdll.dll", "NtQuerySystemInformation", NtQuerySystemInformation))
        throw_runtime_error("Error: bad RtlpHypervisorSharedUserVa");
    ULONG size = 0;
    const ULONG SystemHypervisorSharedPageInformation = 0xc5;
    int ret = NtQuerySystemInformation(SystemHypervisorSharedPageInformation, (void*)&RtlpHypervisorSharedUserVa,
        sizeof(RtlpHypervisorSharedUserVa), &size);
    if (ret < 0 || size != sizeof(RtlpHypervisorSharedUserVa) || !RtlpHypervisorSharedUserVa)
        throw_runtime_error("Error: bad RtlpHypervisorSharedUserVa ret");
    uint32_t RtlpHypervisorSharedUserVa_notZero = *(uint32_t*)RtlpHypervisorSharedUserVa;
    if (!RtlpHypervisorSharedUserVa_notZero)
        throw_runtime_error("Error: bad RtlpHypervisorSharedUserVa[0]");

    RtlpHypervisorSharedUserVa_tscScale = RtlpHypervisorSharedUserVa + 1;
    RtlpHypervisorSharedUserVa_tscBias = reinterpret_cast<volatile int64_t*>(RtlpHypervisorSharedUserVa + 2);
    SharedUserData_qpcShift = (SharedUserData + 0x03c7);
    SharedUserData_qpcBias = reinterpret_cast<volatile int64_t*>(SharedUserData + 0x03b8);

    qpcBias = *SharedUserData_qpcBias;
    tscScale = *RtlpHypervisorSharedUserVa_tscScale;

    LARGE_INTEGER qpf;
    QueryPerformanceFrequency(&qpf);
    if (qpf.QuadPart != FREQ_10MHz)
        throw_runtime_error("Error: bad qpf");
    if (*RtlpHypervisorSharedUserVa_tscBias != 0 || *SharedUserData_qpcShift != 0)
        throw_runtime_error("Error: bad RtlpHypervisorSharedUserVa scale");
}

static unsigned long long qpf()
{
    initTscWin10();
    if (tscScale)
        return static_cast<unsigned long long>((uint128_t(FREQ_10MHz) << 64) / tscScale);
    if (::debug_getQpcFreq() != FREQ_1GHz)
        throw_runtime_error("Error: bad time clock");
    if (auto ret = supports_constant_tsc() ? tsc_frequency_from_cpuid() : 0)
        return ret;
    throw_runtime_error("Error: tsc freq is 0");
}

static unsigned long long qpc()
{
    unsigned tmp;
    return __rdtscp(&tmp);
}
#elif defined(LINUX_TSC)
static unsigned long long qpc()
{
    unsigned tmp;
    return __rdtscp(&tmp);
}

// The kernel's own TSC rate: a perf event's mmap page converts ticks to ns with time_mult and time_shift
static uint64_t tsc_frequency_from_perf()
{
    perf_event_attr attr = {};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_SOFTWARE;
    attr.config = PERF_COUNT_SW_DUMMY;
    attr.exclude_kernel = 1;
    const int fd = static_cast<int>(syscall(SYS_perf_event_open, &attr, 0, -1, -1, 0));
    if (fd < 0)
        return 0;
    const size_t pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    void* page = mmap(nullptr, pageSize, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (page == MAP_FAILED)
        return 0;
    const auto* info = static_cast<const perf_event_mmap_page*>(page);
    uint64_t freq = 0;
    if (info->cap_user_time && info->time_mult != 0)
        freq = static_cast<uint64_t>((uint128_t(FREQ_1GHz) << info->time_shift) / uint64_t(info->time_mult));
    munmap(page, pageSize);
    return freq;
}

static long long monotonicRawNs()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec * FREQ_1GHz + ts.tv_nsec;
}

// A container may refuse perf events and a VM may report no rate in CPUID
static uint64_t tsc_frequency_measured()
{
    const long long ns0 = monotonicRawNs();
    const unsigned long long tsc0 = qpc();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const long long ns1 = monotonicRawNs();
    const unsigned long long tsc1 = qpc();
    return static_cast<uint64_t>(uint128_t(tsc1 - tsc0) * uint64_t(FREQ_1GHz) / static_cast<uint64_t>(ns1 - ns0));
}

static unsigned long long qpf()
{
    if (const uint64_t freq = tsc_frequency_from_perf())
        return freq;
    if (const uint64_t freq = supports_constant_tsc() ? tsc_frequency_from_cpuid() : 0)
        return freq;
    return tsc_frequency_measured();
}
#else
// No TSC to read: the clock is time.cpp's own, so its epoch base applies as it is
static unsigned long long qpf()
{
    ::debug_qpcEpochBase(); // sets the frequency
    return static_cast<unsigned long long>(::debug_getQpcFreq());
}

static unsigned long long qpc()
{
    return static_cast<unsigned long long>(::timeTicks() + ::debug_qpcEpochBase());
}
#endif

static void setFreq()
{
    qpcFreq = qpf();
    qpcFreqInverse = 1.0 / qpcFreq;
    qpcFreqDiv = libdivide::libdivide_u64_branchfree_gen(qpcFreq);
    maxNsInU64 = UINT64_MAX / FREQ_1GHz;
    maxUsInU64 = UINT64_MAX / FREQ_1MHz;
    maxMsInU64 = UINT64_MAX / FREQ_1KHz;
}

void debug_SetFreq(long long freq) // used by unit tests
{
    if (freq == 0)
        freq = qpf();
    qpcFreq = freq;
    qpcFreqInverse = 1.0 / qpcFreq;
    qpcFreqDiv = libdivide::libdivide_u64_branchfree_gen(qpcFreq);
    maxNsInU64 = UINT64_MAX / FREQ_1GHz;
    maxUsInU64 = UINT64_MAX / FREQ_1MHz;
    maxMsInU64 = UINT64_MAX / FREQ_1KHz;
}

long long debug_getQpcFreq()
{
    return qpcFreq;
}

long long debug_qpcEpochBase()
{
#if defined(WINDOWS_TSC) || defined(LINUX_TSC)
    return qpcEpochBase();
#else
    return ::debug_qpcEpochBase();
#endif
}

// the goal of the function is to align values returned by qpc with values returned by ft.
// This way, timeSec will return values very close to values returned by std::time.
// TimeAlignment unit test verifies that this is withing 5 ms.
static long long NOINLINE calcQpcEpochBase()
{
    setFreq();
#if defined(WINDOWS_TSC)
    auto base = int128_t(::debug_qpcEpochBase() - qpcBias);
    base *= qpcFreq;
    base /= FREQ_10MHz;
    return static_cast<long long>(base);
#elif defined(LINUX_TSC)
    timespec ts;
    const long long before = static_cast<long long>(qpc());
    clock_gettime(CLOCK_REALTIME, &ts);
    const long long after = static_cast<long long>(qpc());
    const long long ns = ts.tv_sec * FREQ_1GHz + ts.tv_nsec;
    return before + (after - before) / 2 - static_cast<long long>(int128_t(ns) * qpcFreq / FREQ_1GHz);
#else
    return debug_qpcEpochBase();
#endif
}

// Two threads can make the first call: a LOG in a static initializer starts the logger thread
static long long qpcEpochBase()
{
    static const long long qpcBase = calcQpcEpochBase();
    return qpcBase;
}

#if defined(WINDOWS_TSC)
static FORCEINLINE long long mullhi_s64(long long x, unsigned long long y)
{
#if defined(LIBDIVIDE_MULH_INTRINSICS) || defined(HAS_INT128_T)
    assert((int64_t)y >= 0);
    return libdivide::libdivide_mullhi_s64(x, (int64_t)y);
#else // using checked unsigned mullhi is 2x faster if no intrinsics available (on 32bit x86)
    if (x >= 0)
        return static_cast<long long>(libdivide::libdivide_mullhi_u64((uint64_t)x, y));
    else
    {
        auto r = static_cast<long long>(libdivide::libdivide_mullhi_u64((uint64_t)-x, y));
        assert(r >= 0);
        return -r;
    }
#endif
}
#endif

long long tscToQpc(long long t) // see tscRawToQpcRaw for full calculation
{
#if defined(WINDOWS_TSC)
    auto h = mullhi_s64(t, tscScale);
    assert(*RtlpHypervisorSharedUserVa_tscBias == 0 && *SharedUserData_qpcShift == 0);
    return h;
#elif defined(LINUX_TSC)
    return static_cast<long long>(int128_t(t) * FREQ_1GHz / qpcFreq);
#else
    return t;
#endif
}

long long tscRawToQpcRaw(long long t) // QueryPerformanceCounter uses this logic to covert rdtsc values
{
#if defined(WINDOWS_TSC)
    // full calculation: return (tscToQpc(t) + *RtlpHypervisorSharedUserVa_tscBias + qpcBias) >> *SharedUserData_qpcShift;
    return tscToQpc(t) + qpcBias;
#elif defined(LINUX_TSC)
    return ::debug_qpcEpochBase() + tscToQpc(t - qpcEpochBase()); // both clocks count from the same UTC epoch
#else
    return t;
#endif
}

long long qpcToTsc(long long t)
{
#if defined(WINDOWS_TSC)
    // Less precise 3x faster (15x on 32bit x86): return ::ticksToTime(t) * qpcFreq
    return static_cast<long long>((uint128_t((unsigned long long)t) << 64) / tscScale);
#elif defined(LINUX_TSC)
    return static_cast<long long>(int128_t(t) * qpcFreq / FREQ_1GHz);
#else
    return t;
#endif
}

picosec ticksToPico(long long x)
{
    picosec ret;
    if (qpcFreq < std::numeric_limits<long long>::max() / FREQ_1THz) // check if `(x % qpcFreq) * FREQ_1THz` could overflow long long
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
        sec = ticks / freq;
        ticks = ticks % freq;
    }
    // auto p = ticks * FREQ_1THz_128 + fractionalTicks * FREQ_1THz_128 / denom;
    auto p = ticks * FREQ_1THz_128 + (shiftR ? (fractionalTicks * FREQ_1THz_128 >> shiftR) : (fractionalTicks * FREQ_1THz_128 / denom));
    auto ps = static_cast<long long>(static_cast<uint64_t>(p / freq));
    auto s = static_cast<long long>(sec);
    if (!negative)
        return picosec{s, ps};
    else
        return ps ? picosec{-s - 1, FREQ_1THz - ps} : picosec{-s, 0};
}

static FORCEINLINE unsigned long long convertTicks(unsigned long long x, unsigned FREQ)
{
    if ((FREQ == FREQ_1GHz && x <= maxNsInU64) || (FREQ == FREQ_1MHz && x <= maxUsInU64) || (FREQ == FREQ_1KHz && x <= maxMsInU64) || FREQ == 1)
        return libdivide::libdivide_u64_branchfree_do(FREQ * x, &qpcFreqDiv);
    auto sec = libdivide::libdivide_u64_branchfree_do(x, &qpcFreqDiv);
    auto rem = x - sec * (unsigned long long)qpcFreq;
    return FREQ * sec + libdivide::libdivide_u64_branchfree_do(FREQ * rem, &qpcFreqDiv);
}

static FORCEINLINE long long convertTicks(long long x, unsigned FREQ)
{
#if defined(_M_IX86) || defined(__i386__)
    if (FREQ == 1)
        return x / qpcFreq;
#endif
    if (x >= 0)
        return static_cast<long long>(convertTicks(static_cast<unsigned long long>(x), FREQ));
    else
        return -static_cast<long long>(convertTicks(static_cast<unsigned long long>(-x), FREQ));
}

long long ticksToNano(long long x)
{
    // faster version, but with some errors: return mullhi_s64(x, tscScale * 100);
    return convertTicks(x, FREQ_1GHz);
}

long long ticksToMicro(long long x)
{
    return convertTicks(x, FREQ_1MHz);
}

long long ticksToMs(long long x)
{
    return convertTicks(x, FREQ_1KHz);
}

long long ticksToSec(long long x)
{
    return convertTicks(x, 1);
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

long long rawToTicks(long long raw)
{
    return raw - qpcEpochBase();
}

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

std::string formatTicks(long long ticks_signed)
{
    return formatTicksImpl(ticks_signed, static_cast<unsigned long long>(qpcFreq));
}
}
