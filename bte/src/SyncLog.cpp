#include "SyncLog.h"
#include "DestructionManager.h"
#include "sysutil.h"
#include "date.h"
#include "quill/core/Rdtsc.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <string_view>
#include <thread>
#if defined(_WIN32) || defined(__CYGWIN__)
#include "GetProcAddr.h"
#include <windows.h>
#endif
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <pthread.h>
#endif
#ifdef _WIN32
#include <share.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

bool AsyncLogSilent = false;

static const char kLogFileName[] = "log.txt";

static const char* logFileName()
{
    const char* envName = getenv("LOG_FILE");
    return (envName && envName[0]) ? envName : kLogFileName;
}

uint32_t logThreadId()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    return GetCurrentThreadId();
#elif defined(__linux__)
    return static_cast<uint32_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
    uint64_t id = 0;
    pthread_threadid_np(nullptr, &id);
    return static_cast<uint32_t>(id);
#else
    return 0;
#endif
}

// Plain statics, still usable during static destruction
static constinit std::atomic_flag gLogLock;
static constinit FILE* gLogFile = nullptr;
static constinit bool gLogFileFailed = false; // reported once; a file that never opened is not tried again
static constinit char gFallbackFile[4096] = {}; // the file this process logs to while another process holds its own
static constexpr bool kAppendLogFile = (kLogMode & LogFileAppend) != 0;
static constexpr long long kLogFileMaxBytes = 100LL * 1024 * 1024; // an appended file past it is moved aside
static constinit std::atomic<LogLineHook> gLogLineHook {nullptr};
static constinit bool gLogClosed = false; // under the log's lock
static constinit std::atomic<bool> gWrittenSinceFlush {false};
static constinit std::atomic<uint32_t> gStoppedBy {0}; // the thread that ran stopSyncLog
static constinit std::atomic<LogTransitionHook> gLogTransitionHook {nullptr};

#if defined(_WIN32) || defined(__CYGWIN__)
using ShutdownInProgress = BOOLEAN(NTAPI*)();

static ShutdownInProgress lookUpShutdownInProgress()
{
    ShutdownInProgress inProgress = nullptr;
    GetProcAddr("ntdll.dll", "RtlDllShutdownInProgress", inProgress);
    return inProgress;
}

// Looked up before main: at the exit the lookup would wait for the loader lock
static const ShutdownInProgress gShutdownInProgress = lookUpShutdownInProgress();
#endif

bool processExiting()
{
#if defined(_WIN32) || defined(__CYGWIN__)
    return gShutdownInProgress != nullptr && gShutdownInProgress() != FALSE;
#else
    return false;
#endif
}

void readThreadName(char (&name)[kThreadNameWidth + 1])
{
    name[0] = '\0';
#if defined(_WIN32) || defined(__CYGWIN__)
    PWSTR description = nullptr;
    if (FAILED(GetThreadDescription(GetCurrentThread(), &description)))
        return;
    size_t length = 0;
    for (; length < kThreadNameWidth && description[length] != 0; ++length)
        name[length] = description[length] < 128 ? static_cast<char>(description[length]) : '?';
    name[length] = '\0';
    LocalFree(description);
#else
    // Not the OS's name: a new thread inherits its creator's, so every unnamed thread would carry another's name
    readGivenThreadName(name);
#endif
}

void readGivenThreadName(char (&name)[kThreadNameWidth + 1])
{
    const char* given = givenThreadName();
    size_t length = 0;
    for (; length < kThreadNameWidth && given[length] != 0; ++length)
        name[length] = given[length];
    name[length] = '\0';
}

// Taken once, so log times stay monotonic across clock and daylight saving changes
static double localTimeOffset()
{
    static constinit bool known = false;
    static constinit double offset = 0;
    if (!known)
    {
        const time_t now = time(nullptr);
        tm local {};
        tm utc {};
        localtime_r(&now, &local);
        gmtime_r(&now, &utc);
        int days = local.tm_yday - utc.tm_yday;
        if (local.tm_year != utc.tm_year)
            days = local.tm_year > utc.tm_year ? 1 : -1;
        offset = days * 86400.0 + (local.tm_hour - utc.tm_hour) * 3600.0 + (local.tm_min - utc.tm_min) * 60.0;
        known = true;
    }
    return offset;
}

// "2026-10-14 23:41:07.123   12345 IbClient             ", right-aligned to end at kLogPrefixSize
static size_t printLogPrefix(char* line, double epochSeconds, uint32_t threadId, std::string_view threadName)
{
    const long long ms = static_cast<long long>((epochSeconds + localTimeOffset()) * 1000);
    const long long seconds = ms / 1000;
    const std::chrono::year_month_day date {std::chrono::sys_days {std::chrono::days {seconds / 86400}}};
    const int secondOfDay = static_cast<int>(seconds % 86400);
    const int nameLength = static_cast<int>(std::min(threadName.size(), kThreadNameWidth));
    char prefix[kLogPrefixSize + 16];
    int length = snprintf(prefix, sizeof(prefix), "%04d-%02u-%02u %02d:%02d:%02d.%03d %7u %-*.*s ", int(date.year()),
                          unsigned(date.month()), unsigned(date.day()), secondOfDay / 3600, secondOfDay / 60 % 60,
                          secondOfDay % 60, static_cast<int>(ms % 1000), threadId, static_cast<int>(kThreadNameWidth),
                          nameLength, threadName.data());
    if (length < 0 || length > static_cast<int>(kLogPrefixSize))
        length = 0;
    memcpy(line + kLogPrefixSize - length, prefix, static_cast<size_t>(length));
    return kLogPrefixSize - static_cast<size_t>(length);
}

static constinit thread_local bool tHoldsLogLock = false;
static constinit thread_local bool tCrashFlushing = false;
static constinit std::atomic<long long> gCrashWaitMs {kCrashWaitMs};

// False only on a crash-flushing thread that waited a second
static bool lockLog()
{
    long long waitingSince = 0;
    while (gLogLock.test_and_set(std::memory_order_acquire))
    {
        if (tCrashFlushing)
        {
            const long long now = timeTicks();
            if (waitingSince == 0)
                waitingSince = now;
            else if (ticksToMs(now - waitingSince) >= 1000)
                return false;
        }
        std::this_thread::yield();
    }
    tHoldsLogLock = true;
    return true;
}

static void unlockLog()
{
    tHoldsLogLock = false;
    gLogLock.clear(std::memory_order_release);
}

bool holdsLogLock()
{
    return tHoldsLogLock;
}

void setCrashFlushing(bool flushing)
{
    tCrashFlushing = flushing;
}

bool crashFlushing()
{
    return tCrashFlushing;
}

void setCrashWaitMs(long long ms)
{
    gCrashWaitMs.store(ms, std::memory_order_relaxed);
}

long long crashWaitMs()
{
    return gCrashWaitMs.load(std::memory_order_relaxed);
}

// Under the log's lock; the lines still go to stdout and the debugger
static void reportLogFileFailure(const char* what)
{
    const int error = errno;
    const char* name = gFallbackFile[0] ? gFallbackFile : logFileName();
    if (!gLogFileFailed)
        fprintf(stderr, "log file %s: %s: %s\n", name, what, strerror(error));
    gLogFileFailed = true;
}

// "log.txt" and "1234" give "log-1234.txt" beside it; false if it doesn't fit
static bool nameBeside(const char* name, const char* suffix, char* out, size_t size)
{
    const char* base = name;
    for (const char* c = name; *c; ++c)
    {
        if (*c == '/' || *c == '\\')
            base = c + 1;
    }
    const char* extension = strrchr(base, '.');
    if (extension == nullptr)
        extension = base + strlen(base);
    const int length = snprintf(out, size, "%.*s-%s%s", static_cast<int>(extension - name), name, suffix, extension);
    return length > 0 && static_cast<size_t>(length) < size;
}

// A line starting "2026-10-21 09:40:11" gives "20261021-094011", which sorts as the times do
static bool stampOfLine(const char* line, size_t length, char (&stamp)[16])
{
    static constexpr char kShape[] = "0000-00-00 00:00:00";
    if (length < sizeof(kShape) - 1)
        return false;
    size_t out = 0;
    for (size_t i = 0; i + 1 < sizeof(kShape); ++i)
    {
        const bool digit = line[i] >= '0' && line[i] <= '9';
        if (kShape[i] == '0' ? !digit : line[i] != kShape[i])
            return false;
        if (digit)
            stamp[out++] = line[i];
        if (i == 9)
            stamp[out++] = '-';
    }
    stamp[out] = '\0';
    return true;
}

// "log.txt" gives "log-20261021-094011.txt" beside it: its first line's time, else its last write's
static bool asideName(const char* name, const char* firstBytes, size_t length, time_t written, char* out, size_t size)
{
    char stamp[16];
    if (!stampOfLine(firstBytes, length, stamp))
    {
        tm local {};
        localtime_r(&written, &local);
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    }
    return nameBeside(name, stamp, out, size);
}

// "log.txt" gives "log-20261021-094011-1234.txt" beside it: this run's first line's time, then the process's id
static bool setFallbackFile(const char* name, double epochSeconds)
{
    char prefix[kLogPrefixSize];
    const size_t start = printLogPrefix(prefix, epochSeconds, 0, "");
    char stamp[16];
    char suffix[48];
    if (stampOfLine(prefix + start, kLogPrefixSize - start, stamp))
        snprintf(suffix, sizeof(suffix), "%s-%u", stamp, currentProcessId());
    else
        snprintf(suffix, sizeof(suffix), "%u", currentProcessId());
    if (nameBeside(name, suffix, gFallbackFile, sizeof(gFallbackFile)))
        return true;
    gFallbackFile[0] = '\0';
    return false;
}

#ifdef _WIN32
// Before the open: a file another process holds refuses the rename, and rename never replaces an existing file
static void moveAsideIfLarge(const char* name)
{
    struct _stat64 info {};
    if (_stat64(name, &info) != 0 || info.st_size <= kLogFileMaxBytes)
        return;
    char firstBytes[32];
    size_t length = 0;
    if (FILE* file = _fsopen(name, "rb", _SH_DENYNO))
    {
        length = fread(firstBytes, 1, sizeof(firstBytes), file);
        fclose(file);
    }
    char aside[sizeof(gFallbackFile)];
    if (asideName(name, firstBytes, length, info.st_mtime, aside, sizeof(aside)))
        rename(name, aside);
}
#else
// Once this process holds the file at the name: link refuses an existing name, so another file there stays
static bool moveAside(const char* name, time_t written)
{
    // A descriptor of its own: the log's is write-only
    char firstBytes[32];
    ssize_t length = -1;
    const int reader = open(name, O_RDONLY | O_CLOEXEC);
    if (reader >= 0)
    {
        length = read(reader, firstBytes, sizeof(firstBytes));
        close(reader);
    }
    char aside[sizeof(gFallbackFile)];
    if (!asideName(name, firstBytes, length > 0 ? static_cast<size_t>(length) : 0, written, aside, sizeof(aside)))
        return false;
    return link(name, aside) == 0 && unlink(name) == 0;
}

static FILE* streamOf(int fd)
{
    FILE* file = fdopen(fd, kAppendLogFile ? "ab" : "wb");
    if (file == nullptr)
        close(fd);
    return file;
}
#endif

// Taken exclusively, so that two processes never write one file; nullptr with held set when another process has it
static FILE* openExclusively(const char* name, bool& held, [[maybe_unused]] bool mayMoveAside)
{
#ifdef _WIN32
    // N: a child process inheriting the handle would hold the file after this process ended
    FILE* file = _fsopen(name, kAppendLogFile ? "abN" : "wbN", _SH_DENYWR);
    held = file == nullptr && _doserrno == ERROR_SHARING_VIOLATION;
    return file;
#else
    // Tries again when the name has moved on to a new file by the time the lock is taken, as when moved aside meanwhile
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        const int fd = open(name, O_WRONLY | O_CREAT | O_CLOEXEC | (kAppendLogFile ? O_APPEND : 0), 0666);
        // EBUSY: Cygwin's word for a file a Windows process holds
        held = fd < 0 && errno == EBUSY;
        if (fd < 0)
            return nullptr;
        // A device such as /dev/null is every process's; only a regular file is locked, then emptied unless appended
        struct stat info {};
        if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
            return streamOf(fd);
        logTransition(LogTransition::SyncLogLocking);
        // Only another holder sends this process elsewhere; on a file system without locks the file is written unlocked
        held = flock(fd, LOCK_EX | LOCK_NB) != 0 && errno == EWOULDBLOCK;
        struct stat named {};
        const bool moved =
            !held && (stat(name, &named) != 0 || named.st_ino != info.st_ino || named.st_dev != info.st_dev);
        if (held || moved || (!kAppendLogFile && ftruncate(fd, 0) != 0))
        {
            close(fd);
            if (moved)
                continue;
            return nullptr;
        }
        // Its size now that it is this process's
        if (fstat(fd, &info) == 0 && kAppendLogFile && mayMoveAside && info.st_size > kLogFileMaxBytes &&
            moveAside(name, info.st_mtime))
        {
            close(fd);
            mayMoveAside = false;
            continue;
        }
        return streamOf(fd);
    }
    return nullptr;
#endif
}

#ifdef _WIN32
static void appendUtf8(std::string& out, const wchar_t* text, int length)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
        return;
    const size_t at = out.size();
    out.resize(at + static_cast<size_t>(size));
    WideCharToMultiByte(CP_UTF8, 0, text, length, out.data() + at, size, nullptr, nullptr);
}
#endif

// The executable's full path; not its arguments, which can hold a credential (TradingApp's --telegram token)
static void appendExecutablePath(std::string& out)
{
#ifdef _WIN32
    wchar_t path[MAX_PATH * 4];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    appendUtf8(out, path, static_cast<int>(length));
#elif defined(__linux__) || defined(__CYGWIN__)
    char path[4096];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path));
    if (length > 0)
        out.append(path, static_cast<size_t>(length));
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0)
        out += path;
#endif
}

// Under the log's lock: the first line of the process's run in the file, at its first line's time
static void writeRunStart(double epochSeconds)
{
    std::string line(kLogPrefixSize, ' ');
    line += "==== log start, pid " + std::to_string(currentProcessId()) + ": ";
    appendExecutablePath(line);
    line += '\n';
    char threadName[kThreadNameWidth + 1];
    readThreadName(threadName);
    const size_t start = printLogPrefix(line.data(), epochSeconds, logThreadId(), threadName);
    fwrite(line.data() + start, 1, line.size() - start, gLogFile);
}

// Under the log's lock; a file another process holds stays that process's, and this one logs to a file beside it
static void openLogFile(double epochSeconds)
{
#ifdef _WIN32
    if constexpr (kAppendLogFile)
        moveAsideIfLarge(logFileName());
#endif
    bool held = false;
    gLogFile = openExclusively(logFileName(), held, true);
    if (held && setFallbackFile(logFileName(), epochSeconds))
    {
        gLogFile = openExclusively(gFallbackFile, held, false);
        if (gLogFile != nullptr)
        {
            fprintf(stderr, "log file %s is in use by another process; this one logs to %s\n", logFileName(),
                    gFallbackFile);
        }
    }
    if (gLogFile == nullptr)
        reportLogFileFailure("cannot open");
    else
        writeRunStart(epochSeconds);
}

void writeLogLine(char* line, size_t size, double epochSeconds, uint32_t threadId, std::string_view threadName,
                  bool flush)
{
    if (AsyncLogSilent || !lockLog())
        return;
    if (gLogClosed)
    {
        unlockLog();
        return;
    }
    if (const LogLineHook hook = gLogLineHook.load(std::memory_order_acquire))
    {
        hook(threadId, threadName, std::string_view(line, size).substr(kLogPrefixSize, size - kLogPrefixSize - 1));
        unlockLog();
        return;
    }
    const size_t start = printLogPrefix(line, epochSeconds, threadId, threadName);
    const std::string_view text = std::string_view(line, size).substr(start);
    if constexpr ((kLogMode & (LogFileTruncate | LogFileAppend)) != 0)
    {
        // Never from a crash: opening allocates, and the logger thread opened it ahead at its first poll
        if (gLogFile == nullptr && !gLogFileFailed && !tCrashFlushing)
        {
            logTransition(LogTransition::SyncLogOpen);
            openLogFile(epochSeconds);
        }
        if (gLogFile != nullptr)
        {
            if (fwrite(text.data(), 1, text.size(), gLogFile) != text.size() || (flush && fflush(gLogFile) != 0))
                reportLogFileFailure("cannot write");
        }
    }
#if defined(_WIN32) || defined(__CYGWIN__)
    if constexpr ((kLogMode & LogDebugger) != 0)
    {
        // Without a debugger the call takes a mutex every process of the session shares, and only a viewer reads it
        if (IsDebuggerPresent())
            OutputDebugStringA(text.data());
    }
#endif
    if constexpr ((kLogMode & LogStdout) != 0)
    {
        if (!tCrashFlushing)
            fwrite(text.data(), 1, text.size(), stdout);
    }
    if constexpr ((kLogMode & LogStderr) != 0)
    {
        if (!tCrashFlushing)
            fwrite(text.data(), 1, text.size(), stderr);
    }
    gWrittenSinceFlush.store(true, std::memory_order_relaxed);
    unlockLog();
}

void writeLogLine(std::string& line, double epochSeconds, uint32_t threadId, std::string_view threadName, bool flush)
{
    writeLogLine(line.data(), line.size(), epochSeconds, threadId, threadName, flush);
}

void openLogFileAhead()
{
    if constexpr ((kLogMode & (LogFileTruncate | LogFileAppend)) != 0)
    {
        if (AsyncLogSilent || !lockLog())
            return;
        if (!gLogClosed && gLogFile == nullptr && !gLogFileFailed
            && gLogLineHook.load(std::memory_order_acquire) == nullptr)
        {
            logTransition(LogTransition::SyncLogOpen);
            openLogFile(ticksToTime(timeTicks()));
        }
        unlockLog();
    }
}

// Every later line, on any thread, is dropped
static void closeSyncLog()
{
    logTransition(LogTransition::SyncLogClose);
    // Where the runtime skips thread_local destructors at exit, the closing thread's count is still pending
    writeSyncRepeats(false);
    lockLog();
    if (gLogFile != nullptr && fclose(gLogFile) != 0)
        reportLogFileFailure("cannot flush");
    gLogFile = nullptr;
    gLogClosed = true;
    unlockLog();
    fflush(stdout);
    logTransition(LogTransition::SyncLogClosed);
}

struct SyncLogCloser
{
    ~SyncLogCloser()
    {
        closeSyncLog();
    }
};

// Level 1, below the leak finder's 2, so its report can still be logged
[[maybe_unused]] static const bool gSyncLogCloserRegistered = (DestructionManager::make_shared<SyncLogCloser>(1), true);

void stopSyncLog()
{
    gStoppedBy.store(logThreadId(), std::memory_order_release);
}

bool syncLogStopped()
{
    const uint32_t stoppedBy = gStoppedBy.load(std::memory_order_acquire);
    return stoppedBy != 0 && stoppedBy != logThreadId();
}

bool logdetail::syncLinesDropped()
{
    return syncLogStopped() || processExiting();
}

void setLogLineHook(LogLineHook hook)
{
    gLogLineHook.store(hook, std::memory_order_release);
}

void setLogTransitionHook(LogTransitionHook hook)
{
    gLogTransitionHook.store(hook, std::memory_order_release);
}

void logTransition(LogTransition transition)
{
    if (const LogTransitionHook hook = gLogTransitionHook.load(std::memory_order_acquire))
        hook(transition);
}

void flushSyncLog()
{
    if (!lockLog())
        return;
    gWrittenSinceFlush.store(false, std::memory_order_relaxed);
    if (gLogFile != nullptr && fflush(gLogFile) != 0)
        reportLogFileFailure("cannot flush");
    unlockLog();
    // A crash flushes only the file: a pipe nobody reads or a selection in the console could hold stdio for good
    if (tCrashFlushing)
        return;
    fflush(stdout);
    fflush(stderr);
}

void flushSyncLogIfWritten()
{
    if (gWrittenSinceFlush.load(std::memory_order_relaxed))
        flushSyncLog();
}

void logSync(long long timeTick, std::string_view msg)
{
    if (syncLogStopped())
        return;
    std::string line;
    line.reserve(kLogPrefixSize + 128);
    line.resize(kLogPrefixSize);
    line += msg;

    if (line.back() != '\n')
        line += '\n';
    char threadName[kThreadNameWidth + 1];
    readThreadName(threadName);
    writeLogLine(line, ticksToTime(timeTick), logThreadId(), threadName, false);
}

// Here, not in AsyncLog.cpp: LLOG reads it too, and a program logging only with LLOG links no async logger
uint64_t logdetail::quillCounter()
{
    return quill::detail::rdtsc();
}

static constinit std::atomic<LlogRecordSink> gLlogSink {nullptr};
static constinit std::atomic<void (*)()> gLlogFlush {nullptr};
static constinit std::atomic<long long> gRepeatQuietMs {kRepeatCountQuietMs};
static constinit std::atomic<long long> gRepeatMaxMs {kRepeatCountMaxMs};

void setLlogRedirect(LlogRecordSink sink, void (*flush)())
{
    gLlogFlush.store(flush, std::memory_order_release);
    gLlogSink.store(sink, std::memory_order_release);
}

void setRepeatCountDelays(long long quietMs, long long maxMs)
{
    gRepeatQuietMs.store(quietMs, std::memory_order_relaxed);
    gRepeatMaxMs.store(maxMs, std::memory_order_relaxed);
}

long long repeatCountQuietMs()
{
    return gRepeatQuietMs.load(std::memory_order_relaxed);
}

long long repeatCountMaxMs()
{
    return gRepeatMaxMs.load(std::memory_order_relaxed);
}

static void formatRecord(const unsigned char* record, std::string& text)
{
    uint64_t tag;
    memcpy(&tag, record + sizeof(uint64_t), sizeof(tag));
    logdetail::siteOf(tag).format(record + logdetail::kRecordHeader, text);
}

// The thread's last synchronous LLOG record and the repeats of it counted since; no heap, so the leak finder sees none
struct SyncRepeats
{
    unsigned char last[512] {};
    size_t size = 0; // 0: none kept, as for a record too big for last
    uint64_t count = 0;
    long long firstRepeatAt = 0;

    void writeCount()
    {
        if (count == 0)
            return;
        const uint64_t repeats = count;
        count = 0;
        std::string text;
        formatRecord(last, text);
        text.insert(0, "repeated " + std::to_string(repeats) + (repeats > 1 ? " times: " : " time: "));
        logSync(timeTicks(), text);
    }
};

// No destructor: LLOGs in the thread's own thread_local destructors and in static destruction still find it
static constinit thread_local SyncRepeats tSyncRepeats;
static constinit thread_local bool tSyncRepeatsEnded = false;

// Made at the thread's first synchronous LLOG; once it ran, nobody would write a count, so every LLOG is its own line
struct SyncRepeatsAtExit
{
    ~SyncRepeatsAtExit()
    {
        // ExitProcess may have ended the log lock's holder
        if (!processExiting())
            tSyncRepeats.writeCount();
        tSyncRepeatsEnded = true;
    }
};

void writeSyncRepeats(bool endRun)
{
    tSyncRepeats.writeCount();
    if (endRun)
        tSyncRepeats.size = 0;
}

void writeSyncRepeatsForCrash()
{
    SyncRepeats& repeats = tSyncRepeats;
    if (repeats.count == 0)
        return;
    const uint64_t count = repeats.count;
    repeats.count = 0;
    uint64_t tag;
    memcpy(&tag, repeats.last + sizeof(uint64_t), sizeof(tag));
    const logdetail::Site& site = logdetail::siteOf(tag);
    constexpr size_t kTextMax = 512;
    char line[kLogPrefixSize + kTextMax + 2];
    memset(line, ' ', kLogPrefixSize);
    logdetail::BoundedText text {line + kLogPrefixSize, kTextMax};
    char number[24] {};
    text += "repeated ";
    text += std::string_view(number, std::to_chars(number, number + sizeof(number), count).ptr - number);
    text += count > 1 ? " times: " : " time: ";
    if (site.text == nullptr)
        text += "a LOG";
    else
    {
        text += site.llog ? "LLOG(" : "LOG(";
        logdetail::appendQuoted(text, site.text);
        text += ", ";
        text += site.text + strlen(site.text) + 1;
        text += ')';
    }
    text.markCut();
    const size_t size = kLogPrefixSize + text.size;
    line[size] = '\n';
    line[size + 1] = '\0';
    char name[kThreadNameWidth + 1];
    readGivenThreadName(name);
    writeLogLine(line, size + 1, ticksToTime(timeTicks()), logThreadId(), name, false);
}

// With no async logger, LLOG counts its thread's repeats as LOG does; only their thread writes the count
static void writeLlogRecord(const unsigned char* record, size_t size)
{
    SyncRepeats& repeats = tSyncRepeats;
    if (!tSyncRepeatsEnded)
    {
        thread_local SyncRepeatsAtExit atExit;
        static_cast<void>(atExit);
        // The timestamp aside, an equal record repeats the last one
        const size_t tagged = size - sizeof(uint64_t);
        if (repeats.size == size && memcmp(repeats.last + sizeof(uint64_t), record + sizeof(uint64_t), tagged) == 0)
        {
            const long long now = timeTicks();
            if (repeats.count == 0)
                repeats.firstRepeatAt = now;
            ++repeats.count;
            if (ticksToMs(now - repeats.firstRepeatAt) >= repeatCountMaxMs())
                repeats.writeCount();
            return;
        }
        repeats.writeCount();
        repeats.size = size <= sizeof(repeats.last) ? size : 0;
        memcpy(repeats.last, record, repeats.size);
    }
    std::string text;
    formatRecord(record, text);
    logSync(timeTicks(), text);
}

void logdetail::submitLlog(const unsigned char* record, size_t size)
{
    if (syncLogStopped())
        return;
    if (const LlogRecordSink sink = gLlogSink.load(std::memory_order_acquire))
    {
        writeSyncRepeats(true);
        sink(record, size);
        return;
    }
    // ExitProcess may have ended the log lock's holder where it stood
    if (processExiting())
        return;
    writeLlogRecord(record, size);
}

void logdetail::llogText(std::string_view text)
{
    llogFields(kTextSite, text);
}

void logdetail::llogFlush()
{
    if (syncLinesDropped())
        return;
    writeSyncRepeats(false);
    if (void (*const flush)() = gLlogFlush.load(std::memory_order_acquire))
        flush();
    else
        flushSyncLog();
}

void logdetail::llogFormatted(std::string_view format, std::format_args args)
{
    // Dropped, so not formatted: at exit the CRT's heap can be gone before a detached thread's last LLOG
    if (syncLinesDropped())
        return;
    std::string text;
    formatFmt(text, format, args);
    llogFields(kTextSite, std::string_view(text));
}
