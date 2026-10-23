#include <gtest/gtest.h>
#include "format.h"
#include "sysutil.h"
#include "TestFile.h"
#include "date.h"
#include "AsyncLog.h"
#include "SyncLog.h"
#include "DestructionManager.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <format>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#ifdef _WIN32
static const char kNullDevice[] = "NUL";
#else
static const char kNullDevice[] = "/dev/null";
#endif

using logdetail::FixedString;
using logdetail::literalArgs;
static_assert(literalArgs<FixedString {"%s %d\0\"x\", 5"}, 2>() == std::array<bool, 2> {true, false});
static_assert(literalArgs<FixedString {"%s %s\0\"a, \\\"b\", c"}, 2>() == std::array<bool, 2> {true, false});
static_assert(literalArgs<FixedString {"{}\0f(\"a\", \"b\")"}, 1>() == std::array<bool, 1> {false});
static_assert(literalArgs<FixedString {"{}\0std::pair<int, int> {}"}, 1>() == std::array<bool, 1> {false});
static_assert(literalArgs<FixedString {"%s\0\"BUY\" == side ? label : \"N/A\""}, 1>() == std::array<bool, 1> {false});
static_assert(literalArgs<FixedString {"%s %s\0\"a\" \"b\", \"c\"s"}, 2>() == std::array<bool, 2> {true, false});
static_assert(logdetail::isLiteralSpelling("\"a\" \"b\"") && !logdetail::isLiteralSpelling("\"a\"[0]"));
static_assert(literalArgs<FixedString {"{} {}\0R\"(\", \"fake\", )\", label"}, 2>() ==
              std::array<bool, 2> {false, false});
static_assert(literalArgs<FixedString {"{} {}\0\"a\", u8R\"x(b)x\""}, 2>() == std::array<bool, 2> {false, false});

// The count of a line "repeated N time(s): <line>", which the logger thread may end in " (the last X s earlier)"
static size_t repeatsIn(std::string_view message, std::string_view line)
{
    const std::string_view label = "repeated ";
    if (!message.starts_with(label))
        return 0;
    size_t repeats = 0;
    const char* end = std::from_chars(message.data() + label.size(), message.data() + message.size(), repeats).ptr;
    std::string_view rest = message.substr(static_cast<size_t>(end - message.data()));
    const std::string_view unit = repeats == 1 ? " time: " : " times: ";
    if (!rest.starts_with(unit) || !rest.substr(unit.size()).starts_with(line))
        return 0;
    rest = rest.substr(unit.size() + line.size());
    return rest.empty() || rest.starts_with(" (the last ") ? repeats : 0;
}

// Each test logs in a child process with its own log file (LOG_FILE) and reads the file after the child exits
class LogTest : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!GTEST_FLAG_GET(internal_run_death_test).empty())
            return; // the child
        if (const char* given = getenv("LOG_FILE"))
            givenLogFile = given;
        const std::string name = fmtStr("log-test-%u-%llu.txt", currentProcessId(), (unsigned long long)timeMicro());
        logFile = (std::filesystem::temp_directory_path() / name).string();
        setenv("LOG_FILE", logFile.c_str(), 1);
    }

    void TearDown() override
    {
        if (logFile.empty())
            return;
        remove(logFile.c_str());
        if (givenLogFile.empty())
            unsetenv("LOG_FILE");
        else
            setenv("LOG_FILE", givenLogFile.c_str(), 1);
    }

    std::string logged() const
    {
        std::string content;
        readTestFile(logFile, content);
        return content;
    }

    static size_t count(std::string_view text, std::string_view part)
    {
        size_t found = 0;
        for (size_t at = text.find(part); at != std::string_view::npos; at = text.find(part, at + part.size()))
            ++found;
        return found;
    }

    // The repeats of `line` over all its count lines: a pause of the logging thread splits one run into several
    static size_t repeatsOf(std::string_view text, std::string_view line)
    {
        size_t total = 0;
        for (size_t at = text.find("repeated "); at != std::string_view::npos; at = text.find("repeated ", at + 1))
            total += repeatsIn(text.substr(at, text.find('\n', at) - at), line);
        return total;
    }

    // Written as lines of their own or counted
    static size_t occurrencesOf(std::string_view text, const std::string& line)
    {
        return count(text, line + "\n") - count(text, ": " + line + "\n") + repeatsOf(text, line);
    }

    std::string logFile;
    std::string givenLogFile; // the LOG_FILE the test program was given, back after each test
};

struct LogsInDestructor
{
    const char* text;

    ~LogsInDestructor()
    {
        LOG(text);
    }
};

// A line written synchronously is in the file after the flush; one queued to a logger that ended never gets there
struct LogsAndChecksFile
{
    const char* text;

    ~LogsAndChecksFile()
    {
        LOG(text);
        LOG();
        std::string content;
        readTestFile(getenv("LOG_FILE"), content);
        fputs(content.find(text) != std::string::npos ? "[written]" : "[queued]", stderr);
    }
};

// The process's first LOG, in exit destructors holding Windows' loader lock, which the logger thread it starts needs
static void logOnlyAtExit()
{
    thread_local LogsAndChecksFile atExit {"first line, at a thread's exit"};
    static_cast<void>(atExit);
}

static void logThenExit()
{
    // Constructed before the thread's first LOG, so it is destroyed after Quill releases the thread's queue
    thread_local LogsInDestructor logsAtExit {"logged at thread exit"};
    static_cast<void>(logsAtExit);
    for (int i = 0; i < 1000; ++i)
        LOG("queued line %d", i);
}

static void logBeforeAndAfter(std::atomic<int>* step)
{
    for (int i = 0; i < 3; ++i)
        LOG("thread line before the logger ends");
    step->store(1);
    while (step->load() != 2)
        std::this_thread::yield();
    LOG("thread line after the logger ended");
}

// Its thread logs before the constructor returns, and again when the destructor tells it to
struct LoggingThread
{
    std::atomic<int> step {0};
    std::thread thread {logBeforeAndAfter, &step};

    LoggingThread()
    {
        while (step.load() != 1)
            std::this_thread::yield();
    }

    ~LoggingThread()
    {
        step.store(2);
        thread.join();
    }
};

static std::atomic<int> gStep {0};

static void logThenWait()
{
    for (int i = 0; i < 1000; ++i)
        LOG("waiting thread line %d", i);
    for (int i = 0; i < 3; ++i)
        LOG("waiting thread repeat");
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
}

static void abortOnceLogged()
{
    while (gStep.load() != 1)
        std::this_thread::yield();
    abort();
}

static void logUntilExit()
{
    LOG("detached thread line %d", 0);
    gStep.store(1);
    for (int i = 1;; ++i)
        LOG("detached thread line %d", i);
}

#ifdef _WIN32
static DWORD WINAPI logThenSleep(void*)
{
    for (int i = 0; i < 1000; ++i)
        LOG("terminated thread line %d", i);
    for (int i = 0; i < 3; ++i)
        LOG("terminated thread repeat");
    gStep.store(1);
    Sleep(INFINITE);
    return 0;
}
#endif

static void repeatThenBlock()
{
    for (int i = 0; i < 3; ++i)
        LOG("blocked thread repeat");
    gStep.store(1);
    for (;;)
        std::this_thread::sleep_for(std::chrono::hours(1));
}

static constexpr int kHardThreads = 4;
static std::atomic<bool> gStopHardLogging {false};
static std::atomic<int> gHardLogged[kHardThreads];
static std::atomic<int> gHardStarted {0};

static void logHard(int thread)
{
    int i = 0;
    for (; !gStopHardLogging.load(); ++i)
    {
        LOG("hard thread %d line %d", thread, i);
        // Odd threads also flush, so the logger's end meets them inside Quill
        if (thread % 2 != 0)
            LOG();
        if (i == 0)
            gHardStarted.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    gHardLogged[thread].store(i);
}

// Its threads log until it ends below the logger's level; then it logs how many lines each of them wrote
struct HardLoggers
{
    std::vector<std::thread> threads;

    HardLoggers()
    {
        for (int t = 0; t < kHardThreads; ++t)
            threads.emplace_back(logHard, t);
    }

    ~HardLoggers()
    {
        gStopHardLogging.store(true);
        for (std::thread& thread : threads)
            thread.join();
        for (int t = 0; t < kHardThreads; ++t)
            LOG("hard thread %d logged %d lines", t, gHardLogged[t].load());
    }
};

// How many of the thread's numbered lines the file holds, or -1 if one is missing or repeated before the last
static int hardLinesInOrder(const std::string& text, int thread)
{
    const std::string prefix = fmtStr("hard thread %d line ", thread);
    int expected = 0;
    for (size_t at = text.find(prefix); at != std::string::npos; at = text.find(prefix, at + prefix.size()))
    {
        if (strtol(text.c_str() + at + prefix.size(), nullptr, 10) != expected)
            return -1;
        ++expected;
    }
    return expected;
}

struct Counter
{
    int value;
};

// Formats without iostreams, so it works in static destruction too; std::chrono's formatter does not
template <>
struct std::formatter<Counter> : std::formatter<int>
{
    auto format(const Counter& counter, std::format_context& context) const
    {
        return std::formatter<int>::format(counter.value, context);
    }
};

static void logStatementsWithLog()
{
    LOG("literal line");
    LOG("printf %d %s %.2f", 7, "text", 1.5);
    LOG("fmt {} {} {}", 7, std::string("text"), 1.5);
    LOG("custom {}", Counter {3});
    LOG(std::string("a string"));
}

static void logStatementsWithLlog()
{
    LLOG("literal line");
    LLOG("printf %d %s %.2f", 7, "text", 1.5);
    LLOG("fmt {} {} {}", 7, std::string("text"), 1.5);
    LLOG("custom {}", Counter {3});
    LLOG(std::string("a string"));
}

static const std::vector<std::string> kStatementLines {
    "literal line", "printf 7 text 1.50", "fmt 7 text 1.5", "custom 3", "a string",
};

// The file holds these lines in this order, each a whole line
static bool linesInOrder(const std::string& text, const std::vector<std::string>& lines)
{
    size_t at = 0;
    for (const std::string& line : lines)
    {
        at = text.find(" " + line + "\n", at);
        if (at == std::string::npos)
            return false;
        at += line.size();
    }
    return true;
}

// The exiting thread logs the statements with LLOG, then with LOG, once the async logger has ended
struct LogsStatementsAtExit
{
    ~LogsStatementsAtExit()
    {
        logStatementsWithLlog();
        logStatementsWithLog();
    }
};

// Starts a thread that logs once the logger has ended, then logs from the exiting thread itself
struct LogsFromNewThreadAtExit
{
    ~LogsFromNewThreadAtExit()
    {
        std::thread([] { LOG("new thread after the logger ended"); }).join();
        LOG("exiting thread after the logger ended");
    }
};

static std::atomic<bool> gThreadLocalDestroyed {false};

struct MarksDestroyed
{
    ~MarksDestroyed()
    {
        gThreadLocalDestroyed.store(true);
    }
};

static void holdThreadLocal(std::atomic<int>* step)
{
    thread_local MarksDestroyed marks;
    static_cast<void>(marks);
    step->store(1);
    while (step->load() != 2)
        std::this_thread::yield();
}

static void logThenHoldThreadLocal(std::atomic<int>* step)
{
    LOG("thread line before its end in teardown");
    LOG("thread line before its end in teardown");
    holdThreadLocal(step);
}

// Ends its thread from a static destructor, then tells stderr whether the thread's thread_local destructor ran
struct EndsThreadAtExit
{
    std::atomic<int> step {0};
    std::thread thread;

    explicit EndsThreadAtExit(void (*body)(std::atomic<int>*) = holdThreadLocal) : thread(body, &step)
    {
        while (step.load() != 1)
            std::this_thread::yield();
    }

    ~EndsThreadAtExit()
    {
        step.store(2);
        thread.join();
        fputs(gThreadLocalDestroyed.load() ? "[destroyed]" : "[not destroyed]", stderr);
    }
};

static void logRepeats(int thread)
{
    for (int i = 0; i < 1000; ++i)
        LOG("thread %d says hello", thread);
}

TEST_F(LogTest, RepeatsAreCounted)
{
    EXPECT_EXIT(
    {
        for (int i = 0; i < 1000000; ++i)
            LOG("test hello %d", 1);
        LOG("after the repeats");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "test hello 1"), count(text, ": test hello 1") + 1);
    EXPECT_EQ(repeatsOf(text, "test hello 1"), 999999u);
    EXPECT_LT(text.rfind("repeated "), text.find("after the repeats"));
}

TEST_F(LogTest, RepeatsArePerThread)
{
    EXPECT_EXIT(
    {
        std::thread first(logRepeats, 1);
        std::thread second(logRepeats, 2);
        first.join();
        second.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "thread 1 says hello"), count(text, ": thread 1 says hello") + 1);
    EXPECT_EQ(count(text, "thread 2 says hello"), count(text, ": thread 2 says hello") + 1);
    EXPECT_EQ(repeatsOf(text, "thread 1 says hello"), 999u);
    EXPECT_EQ(repeatsOf(text, "thread 2 says hello"), 999u);
}

TEST_F(LogTest, PrintfArguments)
{
    EXPECT_EXIT(
    {
        char unterminated[4];
        memcpy(unterminated, "abcd", sizeof(unterminated));
        char pointer[32];
        snprintf(pointer, sizeof(pointer), "%p", static_cast<const void*>(unterminated));
        LOG("precision [%.*s] [%.2s]", 3, unterminated, "xyz");
        LOG("logged pointer %p", static_cast<const char*>(unterminated));
        LOG("expected pointer %s", pointer);
        LOG("long %ld, size %zu", 5L, sizeof(int));
        LOG("fmt {} and {}", 7, "text");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "precision [abc] [xy]"), 1u);
    const std::string_view label = "expected pointer ";
    const size_t start = text.find(label);
    ASSERT_NE(start, std::string::npos);
    const size_t end = text.find('\n', start);
    const std::string pointer = text.substr(start + label.size(), end - start - label.size());
    EXPECT_EQ(count(text, "logged pointer " + pointer + "\n"), 1u);
    EXPECT_EQ(count(text, "long 5, size 4"), 1u);
    EXPECT_EQ(count(text, "fmt 7 and text"), 1u);
}

TEST_F(LogTest, BurstLosesNoLines)
{
    EXPECT_EXIT(
    {
        static_cast<void>(freopen(kNullDevice, "w", stdout)); // keeps 100000 lines out of the test's console
        for (int i = 0; i < 100000; ++i)
            LOG("burst line %d", i);
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "burst line "), 100000u);
    EXPECT_EQ(count(text, "burst line 99999\n"), 1u);
}

// Seconds since 1970 of a broken-down time read as UTC
static long long utcSeconds(int year, int month, int day, int hour, int minute, int second)
{
    const long long days = std::chrono::sys_days{std::chrono::year{year} / month / day}.time_since_epoch().count();
    return days * 86400LL + hour * 3600LL + minute * 60LL + second;
}

TEST_F(LogTest, LinesStartWithLocalTime)
{
    EXPECT_EXIT(
    {
        LOG("starts the logger");
        const long long before = ticksToMs(timeTicks());
        LOG("timed line");
        const long long after = ticksToMs(timeTicks());
        LOG("timed between %lld and %lld ms", before, after);
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    const size_t timed = text.find("timed line");
    const size_t between = text.find("timed between ");
    ASSERT_NE(timed, std::string::npos);
    ASSERT_NE(between, std::string::npos);
    long long before = 0, after = 0;
    ASSERT_EQ(sscanf(text.c_str() + between, "timed between %lld and %lld ms", &before, &after), 2);
    const size_t lineStart = text.rfind('\n', timed) + 1;
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, ms = 0;
    ASSERT_EQ(sscanf(text.c_str() + lineStart, "%d-%d-%d %d:%d:%d.%d", &year, &month, &day, &hour, &minute, &second,
                     &ms), 7);
    const time_t now = time(nullptr);
    tm local {};
    localtime_r(&now, &local);
    const long long localOffset = utcSeconds(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour,
                                             local.tm_min, local.tm_sec) - static_cast<long long>(now);
    const long long printedSeconds = utcSeconds(year, month, day, hour, minute, second);
    const long long printed = (printedSeconds - localOffset) * 1000 + ms;
    EXPECT_GE(printed, before - 2);
    EXPECT_LE(printed, after + 2);
}

static void logFromNamedThread()
{
    setThreadName("LogTestThread");
    LOG("named thread %u", logThreadId());
}

static void logAcrossRename()
{
    setThreadName("BeforeRename");
    LOG("before the rename");
    setThreadName("AfterRename");
    LOG("after the rename");
}

TEST_F(LogTest, LinesShowThreadIdAndName)
{
    EXPECT_EXIT(
    {
        std::thread thread(logFromNamedThread);
        thread.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    const size_t at = text.find("named thread ");
    ASSERT_NE(at, std::string::npos);
    const unsigned id = static_cast<unsigned>(strtoul(text.c_str() + at + strlen("named thread "), nullptr, 10));
    EXPECT_EQ(count(text, fmtStr("%7u %-20s named thread %u\n", id, "LogTestThread", id)), 1u);
}

static void logNamedOnce(int number)
{
    setThreadName(fmtStr("short %d", number).c_str());
    LOG("short thread line");
}

static void logBeforeAndAfterShortThreads()
{
    setThreadName("StillRunning");
    LOG("running thread line before");
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
    LOG("running thread line after");
}

// Each ended thread's end record drops its name, and a running thread keeps its own through 1100 of them
TEST_F(LogTest, ManyEndedThreadsLeaveTheRunningOneNamed)
{
    EXPECT_EXIT(
    {
        std::thread running(logBeforeAndAfterShortThreads);
        while (gStep.load() != 1)
            std::this_thread::yield();
        for (int i = 0; i < 1100; ++i)
            std::thread(logNamedOnce, i).join();
        gStep.store(2);
        running.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, fmtStr(" %-20s running thread line after\n", "StillRunning")), 1u);
    EXPECT_EQ(count(text, fmtStr(" %-20s short thread line\n", "short 1099")), 1u);
}

TEST_F(LogTest, RenameTagsTheThreadsNextLines)
{
    EXPECT_EXIT(
    {
        std::thread thread(logAcrossRename);
        thread.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
#ifdef _WIN32
    EXPECT_EQ(count(text, fmtStr("%-20s before the rename\n", "BeforeRename")), 1u);
#endif
    EXPECT_EQ(count(text, fmtStr("%-20s after the rename\n", "AfterRename")), 1u);
}

TEST_F(LogTest, LogAtThreadExitComesAfterQueuedLines)
{
    EXPECT_EXIT(
    {
        std::thread thread(logThenExit);
        thread.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    ASSERT_EQ(count(text, "queued line 999"), 1u);
    ASSERT_EQ(count(text, "logged at thread exit"), 1u);
    EXPECT_LT(text.find("queued line 999"), text.find("logged at thread exit"));
}

TEST_F(LogTest, FlushInTheThreadExitThatStartsTheLogger)
{
#ifdef __CYGWIN__
    GTEST_SKIP() << "Cygwin's emulated TLS frees thread_locals before ending one first made in the exit destructors";
#endif
    EXPECT_EXIT(
    {
        std::thread(logOnlyAtExit).join();
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[written\\]");
    EXPECT_EQ(count(logged(), "first line, at a thread's exit"), 1u);
}

// With the logger running: the line is written, and the thread, gone, is not named as one still holding a queue
TEST_F(LogTest, ThreadsFirstLogInItsExitDestructors)
{
#ifdef __CYGWIN__
    GTEST_SKIP() << "Cygwin's emulated TLS frees thread_locals before ending one first made in the exit destructors";
#endif
    EXPECT_EXIT(
    {
        LOG("main line first");
        std::thread(logOnlyAtExit).join();
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[written\\]");
    const std::string text = logged();
    EXPECT_EQ(count(text, "first line, at a thread's exit"), 1u);
    EXPECT_EQ(count(text, "threads still holding a log queue"), 0u);
}

TEST_F(LogTest, FirstLogFromGlobalDestructor)
{
    EXPECT_EXIT(
    {
        static LogsInDestructor logs {"first log from a global destructor"};
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_EQ(count(logged(), "first log from a global destructor"), 1u);
}

TEST_F(LogTest, FirstLogFromDestructionManager)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(15, std::make_shared<LogsInDestructor>("first log at level 15"));
        DestructionManager::manage(5, std::make_shared<LogsInDestructor>("then at level 5"));
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "first log at level 15"), 1u);
    EXPECT_EQ(count(text, "then at level 5"), 1u);
}

TEST_F(LogTest, FirstLogBelowLoggerLevel)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(9, std::make_shared<LogsAndChecksFile>("first log at level 9"));
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[written\\]");
    EXPECT_EQ(count(logged(), "first log at level 9"), 1u);
}

TEST_F(LogTest, FirstLogThenExit)
{
    EXPECT_EXIT(
    {
        LOG("first log, then exit");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_EQ(count(logged(), "first log, then exit"), 1u);
}

TEST_F(LogTest, FirstLogThenAbort)
{
    EXPECT_DEATH(
    {
        LOG("first log, then abort");
        std::abort();
    },
    "");
    EXPECT_EQ(count(logged(), "first log, then abort"), 1u);
}

TEST_F(LogTest, FirstLogThenCrash)
{
    EXPECT_DEATH(
    {
        LOG("first log, then a crash");
        *(volatile int*)nullptr = 0;
    },
    "");
    EXPECT_EQ(count(logged(), "first log, then a crash"), 1u);
}

enum class Venue { Nyse, Nasdaq };

// The logger thread formats the line, so it throws there
template <>
struct std::formatter<Venue> : std::formatter<std::string_view>
{
    auto format(Venue venue, std::format_context& context) const
    {
        if (venue == Venue::Nyse)
            return std::formatter<std::string_view>::format("NYSE", context);
        throw std::out_of_range("unknown venue");
    }
};

enum class Side { Buy, Sell };

// The logger thread formats the line, so it faults there
template <>
struct std::formatter<Side> : std::formatter<int>
{
    auto format(Side side, std::format_context& context) const
    {
        const volatile int* none = nullptr;
        return std::formatter<int>::format(*none + static_cast<int>(side), context);
    }
};

TEST_F(LogTest, ThrowingFormatterWritesItsLog)
{
    EXPECT_EXIT(
    {
        LOG("venue {}", static_cast<Venue>(7));
        LOG("after the throw");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "LOG(\"venue {}\", static_cast<Venue>(7)) failed: unknown venue; values: 7\n"), 1u);
    EXPECT_EQ(count(text, "after the throw\n"), 1u);
}

TEST_F(LogTest, FaultingFormatterWritesItsLog)
{
    EXPECT_DEATH(
    {
        LOG("side {}", Side::Sell);
        LOG();
    },
    "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "LOG(\"side {}\", Side::Sell) failed: "), 1u);
    EXPECT_EQ(count(text, "; values: 1\n"), 1u);
}

TEST_F(LogTest, FirstLogsOfThreadsThenExit)
{
    EXPECT_EXIT(
    {
        std::vector<std::thread> threads;
        for (int i = 0; i < 4; ++i)
            threads.emplace_back([i] { LOG("first log of thread %d", i); });
        for (std::thread& thread : threads)
            thread.join();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_EQ(count(logged(), "first log of thread "), 4u);
}

TEST_F(LogTest, LogAfterLoggerEnded)
{
    EXPECT_EXIT(
    {
        LOG("before the logger ends");
        DestructionManager::manage(5, std::make_shared<LogsInDestructor>("after the logger ended"));
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "before the logger ends"), 1u);
    EXPECT_EQ(count(text, "after the logger ended"), 1u);
    EXPECT_LT(text.find("before the logger ends"), text.find("after the logger ended"));
}

TEST_F(LogTest, OtherThreadLogsAfterLoggerEnded)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(5, std::make_shared<LoggingThread>());
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "thread line before the logger ends"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: thread line before the logger ends"), 1u);
    EXPECT_EQ(count(text, "thread line after the logger ended"), 0u);
    EXPECT_LT(text.find("thread line before the logger ends"), text.find("repeated 2 times"));
    EXPECT_EQ(count(text, "threads still holding a log queue at the logger's end: "), 1u);
}

TEST_F(LogTest, NewThreadLogsNothingAfterLoggerEnded)
{
    EXPECT_EXIT(
    {
        LOG("before the logger ends");
        DestructionManager::manage(5, std::make_shared<LogsFromNewThreadAtExit>());
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "before the logger ends"), 1u);
    EXPECT_EQ(count(text, "new thread after the logger ended"), 0u);
    EXPECT_EQ(count(text, "exiting thread after the logger ended"), 1u);
}

TEST_F(LogTest, LogAfterTheLogClosedIsDropped)
{
    EXPECT_EXIT(
    {
        LOG("before the log closes");
        DestructionManager::destroyNow();
        LOG("after the log closed");
        LLOG("llog after the log closed");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "before the log closes"), 1u);
    EXPECT_EQ(count(text, "after the log closed"), 0u);
}

TEST_F(LogTest, LlogWritesWhatLogWritesWithoutTheAsyncLogger)
{
    EXPECT_EXIT(
    {
        logStatementsWithLlog();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_TRUE(linesInOrder(logged(), kStatementLines));
}

TEST_F(LogTest, LlogWritesWhatLogWritesAfterTheAsyncLoggerEnded)
{
    EXPECT_EXIT(
    {
        LOG("before the logger ends");
        DestructionManager::manage(5, std::make_shared<LogsStatementsAtExit>());
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    std::vector<std::string> twice = kStatementLines;
    twice.insert(twice.end(), kStatementLines.begin(), kStatementLines.end());
    EXPECT_TRUE(linesInOrder(logged(), twice));
}

// Without a LOG the async logger never starts, and LLOG writes and counts its repeats itself
TEST_F(LogTest, LlogWithoutTheAsyncLogger)
{
    EXPECT_EXIT(
    {
        for (int i = 0; i < 3; ++i)
            LLOG("same %d", 1);
        LLOG("fmt {} {}", 2, std::string("two"));
        LLOG("custom {}", Counter {3});
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "same 1\n"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: same 1\n"), 1u);
    EXPECT_EQ(count(text, "fmt 2 two\n"), 1u);
    EXPECT_EQ(count(text, "custom 3\n"), 1u);
    EXPECT_LT(text.find("repeated 2 times: same 1"), text.find("fmt 2 two"));
}

// Takes the lock Quill's backend takes at its start (BackendWorkerLock), so the start fails
static void holdQuillBackendLock()
{
#ifdef _WIN32
    CreateMutexA(nullptr, TRUE, ("Local\\QuillBackendLock" + std::to_string(GetCurrentProcessId())).c_str());
#else
    const std::string path = "/tmp/QuillBackendLock" + std::to_string(getpid());
    flock(open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644), LOCK_EX);
#endif
}

static void keepTerminate()
{
    abort();
}

// The logger's end removes no handler a failed start never installed
struct ChecksTerminateHandler
{
    ~ChecksTerminateHandler()
    {
        fputs(std::get_terminate() == keepTerminate ? "[kept]" : "[replaced]", stderr);
    }
};

TEST_F(LogTest, FailedStartLogsSynchronously)
{
#ifdef __CYGWIN__
    GTEST_SKIP() << "Backend::start throws inside std::call_once, which on Cygwin ends the process instead";
#endif
    EXPECT_EXIT(
    {
        holdQuillBackendLock();
        std::set_terminate(keepTerminate);
        DestructionManager::manage(5, std::make_shared<ChecksTerminateHandler>());
        LOG("first line");
        LOG("second line");
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[kept\\]");
    const std::string text = logged();
    EXPECT_EQ(count(text, "the async logger did not start: "), 1u);
    EXPECT_TRUE(linesInOrder(text, {"first line", "second line"}));
}

TEST_F(LogTest, LogFlushWritesTheSyncRepeatCount)
{
    EXPECT_EXIT(
    {
        for (int i = 0; i < 3; ++i)
            LLOG("startup retry");
        LOG();
        std::string content;
        readTestFile(getenv("LOG_FILE"), content);
        fputs(content.find("repeated 2 times: startup retry") != std::string::npos ? "[counted]" : "[pending]", stderr);
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[counted\\]");
}

TEST_F(LogTest, SyncRepeatCountComesBeforeTheFirstLog)
{
    EXPECT_EXIT(
    {
        for (int i = 0; i < 3; ++i)
            LLOG("before the logger starts");
        LOG("ready");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_TRUE(linesInOrder(logged(),
                             {"before the logger starts", "repeated 2 times: before the logger starts", "ready"}));
}

// After the logger's end: a LOG between equal LLOGs ends their run, so the LLOG after it is a line of its own
struct LogsSyncRunAroundLog
{
    ~LogsSyncRunAroundLog()
    {
        const std::string_view run = "sync run";
        LLOG(run);
        LLOG(run);
        LOG("a different line");
        LLOG(run);
        LLOG("");
    }
};

TEST_F(LogTest, LogEndsTheSyncRepeatRun)
{
    EXPECT_EXIT(
    {
        LOG("before the logger ends");
        DestructionManager::manage(5, std::make_shared<LogsSyncRunAroundLog>());
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_TRUE(linesInOrder(text, {"sync run", "repeated 1 time: sync run", "a different line", "sync run"}));
    EXPECT_EQ(count(text, "repeated 1 time: sync run"), 1u);
}

struct LogsSyncRunInDestructor
{
    const char* text;

    ~LogsSyncRunInDestructor()
    {
        for (int i = 0; i < 3; ++i)
            LLOG("%s", text);
    }
};

// The thread_local is destroyed after the thread's synchronous count was written, the teardown after every thread_local
TEST_F(LogTest, SyncRepeatsAtExitAreAllWritten)
{
    EXPECT_EXIT(
    {
        thread_local LogsSyncRunInDestructor beforeTheFirstLlog {"thread_local made before the first llog"};
        static_cast<void>(beforeTheFirstLlog);
        LLOG("first llog");
        DestructionManager::manage(5, std::make_shared<LogsSyncRunInDestructor>("in the teardown"));
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(occurrencesOf(text, "thread_local made before the first llog"), 3u)
        << "the main thread's thread_local destructors ran after the statics', after the log closed, as MINGW64's did "
           "before MSYS2's mingw-w64 r495 and gcc 16.2.0-6: update the runtime";
    EXPECT_EQ(occurrencesOf(text, "in the teardown"), 3u);
}

// Fails once a runtime listed here runs them; AsyncLog keeps a thread's ThreadQueue on the heap, so skipping them leaks
TEST(RuntimeTest, ThreadLocalDestructorsOfThreadEndingAtExit)
{
#if defined(__MINGW32__) && (defined(__MINGW64__) || defined(_UCRT))
    const char* expected = "\\[not destroyed\\]";
#else
    const char* expected = "\\[destroyed\\]";
#endif
    EXPECT_EXIT(
    {
        static EndsThreadAtExit endsThread;
        exit(0);
    },
    testing::ExitedWithCode(0), expected);
}

// Whether or not the runtime runs the thread's thread_local destructors there, its line and its count are written
TEST_F(LogTest, ThreadEndingInDestructionManagerLosesNoLines)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(15, std::make_shared<EndsThreadAtExit>(logThenHoldThreadLocal));
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "thread line before its end in teardown"), 2u);
    EXPECT_EQ(count(text, "repeated 1 time: thread line before its end in teardown"), 1u);
}

// mingw-w64 and Cygwin run no thread_local destructor of a thread ending in DestructionManager's teardown
TEST_F(LogTest, ThreadEndingBeforeLoggerEnds)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(15, std::make_shared<LoggingThread>());
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "thread line before the logger ends"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: thread line before the logger ends"), 1u);
    EXPECT_EQ(count(text, "thread line after the logger ended"), 1u);
}

TEST_F(LogTest, CrashOnAnotherThreadWritesQueuedLines)
{
    EXPECT_DEATH(
    {
        std::thread waiting(logThenWait);
        std::thread(abortOnceLogged).join();
        waiting.join();
    },
    "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "waiting thread line "), 1000u);
    EXPECT_EQ(count(text, "waiting thread repeat"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: waiting thread repeat"), 1u);
}

static void logAroundCrashFlush()
{
    for (int i = 0; i < 3; ++i)
        LOG("thread line before the crash");
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
    setThreadName("renamed");
    LOG("thread line after the crash");
}

// A crash the program outlives: its lines and counts are in the file once the flush returns, and every thread logs on
TEST_F(LogTest, LoggerGoesOnAfterACrashFlush)
{
    EXPECT_EXIT(
    {
        std::thread thread(logAroundCrashFlush);
        while (gStep.load() != 1)
            std::this_thread::yield();
        flushAsyncLogForCrash();
        std::string content;
        readTestFile(getenv("LOG_FILE"), content);
        const bool counted = content.find("repeated 2 times: thread line before the crash") != std::string::npos;
        fputs(counted ? "[written]" : "[queued]", stderr);
        gStep.store(2);
        thread.join();
        LOG("main line after the crash");
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[written\\]");
    const std::string text = logged();
    EXPECT_EQ(occurrencesOf(text, "thread line before the crash"), 3u);
    EXPECT_EQ(count(text, "thread line after the crash"), 1u);
    EXPECT_EQ(count(text, "main line after the crash"), 1u);
}

// Returns, so the program goes on
static void recoveringAbortHandler(int)
{
}

// A handler installed before ours recovers from the abort: the flush runs, then the logger and every thread go on
TEST_F(LogTest, RecoveredCrashLetsTheLoggerGoOn)
{
    EXPECT_EXIT(
    {
        signal(SIGABRT, recoveringAbortHandler);
        std::thread thread(logAroundCrashFlush);
        while (gStep.load() != 1)
            std::this_thread::yield();
        raise(SIGABRT);
        gStep.store(2);
        thread.join();
        LOG("main line after the recovered crash");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(occurrencesOf(text, "thread line before the crash"), 3u);
    EXPECT_EQ(count(text, "thread line after the crash"), 1u);
    EXPECT_EQ(count(text, "main line after the recovered crash"), 1u);
}

// Holds stdout's lock, as a thread crashing inside a printf does
static void lockStdout()
{
#ifdef _WIN32
    _lock_file(stdout);
#else
    flockfile(stdout);
#endif
}

// The crash flush has the logger thread write its line, which then waits for stdout forever; the crash still ends
TEST_F(LogTest, CrashWhileTheLineWriterIsStuckEnds)
{
    EXPECT_DEATH(
    {
        setCrashWaitMs(200);
        LOG("line before the crash");
        LOG();
        lockStdout();
        LOG("line the logger thread cannot write");
        abort();
    },
    "");
    EXPECT_EQ(count(logged(), "line before the crash"), 1u);
}

static void endingAbortHandler(int)
{
    static const char text[] = "[earlier handler ran]";
#ifdef _WIN32
    _write(2, text, sizeof(text) - 1);
#else
    static_cast<void>(write(STDERR_FILENO, text, sizeof(text) - 1));
#endif
    _exit(1);
}

// Nobody reads the pipe, so the logger thread ends up blocked writing stdout, holding stdout's lock and the log's
static void sendStdoutToAPipeNobodyReads()
{
    int ends[2];
#ifdef _WIN32
    if (_pipe(ends, 4096, _O_BINARY) == 0)
        _dup2(ends[1], 1);
#else
    if (pipe(ends) == 0)
        dup2(ends[1], STDOUT_FILENO);
#endif
}

// A crash on another thread than the blocked logger thread leaves stdout alone and goes on to the earlier handler
TEST_F(LogTest, CrashWhileStdoutIsBlockedGoesOn)
{
    EXPECT_EXIT(
    {
        setCrashWaitMs(200);
        signal(SIGABRT, endingAbortHandler);
        sendStdoutToAPipeNobodyReads();
        for (int i = 0; i < 4000; ++i)
            LOG("line for a pipe nobody reads %d", i);
        raise(SIGABRT);
    },
    testing::ExitedWithCode(1), "earlier handler ran");
}

#ifdef _WIN32
static LONG WINAPI earlierFilter(EXCEPTION_POINTERS*)
{
    fputs("[earlier handler ran]", stderr);
    fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#else
static void earlierHandler(int sig)
{
    static const char text[] = "[earlier handler ran]";
    static_cast<void>(write(STDERR_FILENO, text, sizeof(text) - 1));
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

// Installed before the first LOG, as a dump writer is, so the log's crash handlers hand on to it
static void installEarlierFaultHandler()
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(earlierFilter);
#else
    signal(SIGSEGV, earlierHandler);
#endif
}

static volatile int* volatile gNowhere = nullptr;

// On another thread: gtest catches a structured exception in a death test's own statement
TEST_F(LogTest, FaultGoesOnToTheEarlierHandler)
{
    EXPECT_DEATH(
    {
        installEarlierFaultHandler();
        LOG("line before the fault");
        std::thread([] { *gNowhere = 1; }).join();
    },
    "earlier handler ran");
    EXPECT_EQ(count(logged(), "line before the fault"), 1u);
}

static void earlierTerminate()
{
    fputs("[earlier terminate handler ran]", stderr);
    fflush(stderr);
    abort();
}

TEST_F(LogTest, TerminateGoesOnToTheEarlierHandler)
{
    EXPECT_DEATH(
    {
        std::set_terminate(earlierTerminate);
        LOG("line before terminate");
        std::terminate();
    },
    "earlier terminate handler ran");
    EXPECT_EQ(count(logged(), "line before terminate"), 1u);
}

static void growQueueThenWait()
{
    for (int i = 0; i < 2000; ++i)
        LOG("growing thread line %d", i);
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
}

// A throw flushes twice: a thread ending between the two leaves no queue listed that Quill freed (ASan sees it)
TEST_F(LogTest, ThreadEndingBetweenTwoCrashFlushes)
{
    EXPECT_EXIT(
    {
        setLogQueueCapacity(2 * 1024, 16 * 1024);
        std::thread thread(growQueueThenWait);
        while (gStep.load() != 1)
            std::this_thread::yield();
        flushAsyncLogForCrash();
        gStep.store(2);
        thread.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        flushAsyncLogForCrash();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_EQ(count(logged(), "growing thread line "), 2000u);
}

#if defined(_MSC_VER) && defined(_DEBUG)
static void logAfterReport()
{
    LOG("thread line after the report");
}

// A CRT report the program goes past leaves every thread logging
TEST_F(LogTest, CrtReportLeavesTheLoggerRunning)
{
    EXPECT_EXIT(
    {
        LOG("main line before the report");
        _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
        _CrtDbgReport(_CRT_WARN, nullptr, 0, nullptr, "a warning the program goes past\n");
        std::thread(logAfterReport).join();
        LOG("main line after the report");
        exit(0);
    },
    testing::ExitedWithCode(0), "a warning the program goes past");
    const std::string text = logged();
    EXPECT_EQ(count(text, "thread line after the report"), 1u);
    EXPECT_EQ(count(text, "main line after the report"), 1u);
}
#endif

// The thread writes a line too big for its queue itself, so the equal lines around it are no repeats of each other
TEST_F(LogTest, LineTooBigForTheQueueEndsARunOfRepeats)
{
    const std::string big(20000, 'b');
    EXPECT_EXIT(
    {
        setLogQueueCapacity(2 * 1024, 16 * 1024);
        LOG("line around a big one");
        LOG("big {}", big);
        LOG("line around a big one");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_TRUE(linesInOrder(text, {"line around a big one", "big " + big, "line around a big one"}));
    EXPECT_EQ(repeatsOf(text, "line around a big one"), 0u);
}

static std::atomic<bool> gHoldFirstRead {false};

static void holdFirstRead(LogTransition transition)
{
    while (transition == LogTransition::AsyncFirstRead && gHoldFirstRead.load())
        std::this_thread::yield();
}

// Before the logger thread reads, the thread writes its queued lines ahead of the big line and keeps its name after
TEST_F(LogTest, LineTooBigForTheQueueBeforeTheLoggerReads)
{
    const std::string big(20000, 'b');
    EXPECT_EXIT(
    {
        setLogQueueCapacity(2 * 1024, 16 * 1024);
        setThreadName("TooBigLine");
        gHoldFirstRead.store(true);
        setLogTransitionHook(holdFirstRead);
        LOG("queued before the big line");
        LOG("big {}", big);
        LOG("queued after the big line");
        gHoldFirstRead.store(false);
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_TRUE(linesInOrder(text, {"queued before the big line", "big " + big, "queued after the big line"}));
    EXPECT_EQ(count(text, fmtStr(" %-20s queued after the big line\n", "TooBigLine")), 1u);
}

#ifdef _WIN32
TEST_F(LogTest, TerminatedThreadLinesAreWritten)
{
    EXPECT_EXIT(
    {
        HANDLE thread = CreateThread(nullptr, 0, logThenSleep, nullptr, 0, nullptr);
        while (gStep.load() != 1)
            std::this_thread::yield();
        TerminateThread(thread, 0);
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
        LOG("after the termination");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "terminated thread line "), 1000u);
    EXPECT_EQ(count(text, "terminated thread repeat"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: terminated thread repeat"), 1u);
    EXPECT_EQ(count(text, "after the termination"), 1u);
}
#endif

TEST_F(LogTest, BlockedThreadRepeatsAreWrittenAtExit)
{
    EXPECT_EXIT(
    {
        std::thread(repeatThenBlock).detach();
        while (gStep.load() != 1)
            std::this_thread::yield();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "blocked thread repeat"), 2u);
    EXPECT_EQ(count(text, "repeated 2 times: blocked thread repeat"), 1u);
}

TEST_F(LogTest, ThreadsLoggingThroughTheLoggersEndLeaveNoGaps)
{
    EXPECT_EXIT(
    {
        DestructionManager::manage(5, std::make_shared<HardLoggers>());
        while (gHardStarted.load() != kHardThreads)
            std::this_thread::yield();
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    for (int t = 0; t < kHardThreads; ++t)
    {
        const std::string summary = fmtStr("hard thread %d logged ", t);
        const size_t at = text.find(summary);
        ASSERT_NE(at, std::string::npos);
        const long lines = strtol(text.c_str() + at + summary.size(), nullptr, 10);
        const int written = hardLinesInOrder(text, t);
        EXPECT_GT(written, 0) << "thread " << t;
        EXPECT_LE(written, lines) << "thread " << t;
    }
    EXPECT_EQ(count(text, "threads still holding a log queue at the logger's end: "), 1u);
}

TEST_F(LogTest, DetachedThreadLogsWhileProcessExits)
{
    EXPECT_EXIT(
    {
        static_cast<void>(freopen(kNullDevice, "w", stdout)); // keeps the thread's lines out of the test's console
        std::thread(logUntilExit).detach();
        while (gStep.load() != 1)
            std::this_thread::yield();
        LOG("main thread exits");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "main thread exits"), 1u);
    EXPECT_EQ(count(text, "detached thread line 0\n"), 1u);
}

TEST_F(LogTest, LogFileThatCannotOpenIsReported)
{
    const std::string missing = (std::filesystem::temp_directory_path() / "no such dir" / "log.txt").string();
    EXPECT_EXIT(
    {
        // Only in the child: a parent that had not opened its own log file yet would fail on it for the rest of the run
        setenv("LOG_FILE", missing.c_str(), 1);
        static_cast<void>(freopen(kNullDevice, "w", stdout));
        LOG("first line");
        LOG("second line");
        exit(0);
    },
    testing::ExitedWithCode(0), "cannot open");
}

#if defined(__linux__) || defined(__CYGWIN__)
// /dev/full takes the buffered lines and fails their flush
TEST_F(LogTest, LogFileThatCannotFlushIsReported)
{
    setenv("LOG_FILE", "/dev/full", 1);
    EXPECT_EXIT(
    {
        static_cast<void>(freopen(kNullDevice, "w", stdout));
        LOG("first line");
        LOG();
        exit(0);
    },
    testing::ExitedWithCode(0), "cannot flush");
}
#endif

// The file a process logs to while another process holds its log file: "<stem>-<pid><extension>" beside it
static std::filesystem::path fallbackFileOf(const std::filesystem::path& logFile)
{
    const std::filesystem::path::string_type prefix = logFile.stem().native() + std::filesystem::path::value_type('-');
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(logFile.parent_path()))
    {
        if (entry.path().filename().native().starts_with(prefix))
            return entry.path();
    }
    return {};
}

// "<stem>-20261021-094011-1234.txt": the run's first line's time, then its process's id
static bool isDatedPidName(const std::filesystem::path& file, const std::filesystem::path& logFile)
{
    const std::string name = file.filename().string();
    const std::string stem = logFile.stem().string() + "-";
    if (!name.starts_with(stem) || !name.ends_with(".txt") || name.size() < stem.size() + 21)
        return false;
    const std::string_view rest = std::string_view(name).substr(stem.size(), name.size() - stem.size() - 4);
    for (size_t i = 0; i < rest.size(); ++i)
    {
        const bool dash = i == 8 || i == 15;
        if (dash ? rest[i] != '-' : rest[i] < '0' || rest[i] > '9')
            return false;
    }
    return true;
}

[[maybe_unused]] static std::filesystem::path datedPidFileOf(const std::filesystem::path& logFile)
{
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(logFile.parent_path()))
    {
        if (isDatedPidName(entry.path(), logFile))
            return entry.path();
    }
    return {};
}

// A previous run's log past 100 MB, its first line from 2026-01-02 03:04:05
static void writeLargeLog(const std::string& path)
{
    FILE* file = fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    fputs("2026-01-02 03:04:05.678       1                      old run line\n", file);
    fclose(file);
    std::filesystem::resize_file(path, 100 * 1024 * 1024 + 1);
}

// Holds the file as SyncLog does, with a line in it, so that another process logging to it finds it in use
static FILE* holdLogFile(const std::string& path)
{
#ifdef _WIN32
    FILE* file = _fsopen(path.c_str(), "wb", _SH_DENYWR);
#else
    FILE* file = fopen(path.c_str(), "wb");
    if (file != nullptr && flock(fileno(file), LOCK_EX) != 0)
    {
        fclose(file);
        file = nullptr;
    }
#endif
    if (file != nullptr)
    {
        fputs("the holder's line\n", file);
        fflush(file);
    }
    return file;
}

// The logger thread flushes the file when idle, with no LOG() asking for it
TEST_F(LogTest, IdleLoggerThreadFlushesTheFile)
{
    EXPECT_EXIT(
    {
        LOG("line flushed while idle");
        bool flushed = false;
        for (int i = 0; i < 300 && !flushed; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::string content;
            readTestFile(getenv("LOG_FILE"), content);
            flushed = content.find("line flushed while idle") != std::string::npos;
        }
        fputs(flushed ? "[flushed]" : "[not flushed]", stderr);
        exit(0);
    },
    testing::ExitedWithCode(0), "\\[flushed\\]");
}

TEST_F(LogTest, LogFileHeldByAnotherProcessIsLeftToIt)
{
    // The child runs the test again up to EXPECT_EXIT, without a logFile of its own
    FILE* holder = logFile.empty() ? nullptr : holdLogFile(logFile);
    EXPECT_EXIT(
    {
        LOG("first line");
        exit(0);
    },
    testing::ExitedWithCode(0), "is in use by another process");
    ASSERT_NE(holder, nullptr);
    fclose(holder);
    EXPECT_EQ(logged(), "the holder's line\n");
    const std::filesystem::path fallback = fallbackFileOf(logFile);
    ASSERT_FALSE(fallback.empty());
    EXPECT_TRUE(isDatedPidName(fallback, logFile)) << fallback.string();
    std::string content;
    readTestFile(fallback.string(), content);
    remove(fallback.string().c_str());
    EXPECT_EQ(count(content, "first line\n"), 1u);
}

// Held when it is past 100 MB: the holder's file stays where it is, and the run goes to its own
TEST_F(LogTest, LargeLogFileAnotherProcessHoldsStays)
{
    FILE* holder = logFile.empty() ? nullptr : holdLogFile(logFile);
    if (holder != nullptr)
    {
#ifdef _WIN32
        _chsize_s(_fileno(holder), 100 * 1024 * 1024 + 1);
#else
        static_cast<void>(ftruncate(fileno(holder), 100 * 1024 * 1024 + 1));
#endif
    }
    EXPECT_EXIT(
    {
        LOG("run beside the held file");
        exit(0);
    },
    testing::ExitedWithCode(0), "is in use by another process");
    ASSERT_NE(holder, nullptr);
    fclose(holder);
    EXPECT_GT(std::filesystem::file_size(logFile), 100u * 1024 * 1024);
    const std::filesystem::path fallback = fallbackFileOf(logFile);
    ASSERT_FALSE(fallback.empty());
    EXPECT_TRUE(isDatedPidName(fallback, logFile)) << fallback.string();
    std::string content;
    readTestFile(fallback.string(), content);
    remove(fallback.string().c_str());
    EXPECT_EQ(count(content, "run beside the held file\n"), 1u);
}

TEST_F(LogTest, EachRunIsAppendedAfterItsStartLine)
{
    EXPECT_EXIT(
    {
        LOG("first run line");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    EXPECT_EXIT(
    {
        LOG("second run line");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    const std::string_view start = "==== log start, pid ";
    const size_t firstStart = text.find(start);
    const size_t firstLine = text.find("first run line");
    const size_t secondStart = text.find(start, firstStart + 1);
    const size_t secondLine = text.find("second run line");
    ASSERT_NE(secondLine, std::string::npos);
    EXPECT_LT(firstStart, firstLine);
    EXPECT_LT(firstLine, secondStart);
    EXPECT_LT(secondStart, secondLine);
    // The executable's full path, without the arguments that made it a death test's child
    const size_t path = text.find(": ", firstStart) + 2;
#ifdef _WIN32
    EXPECT_EQ(text.compare(path + 1, 2, ":\\"), 0);
#else
    EXPECT_EQ(text[path], '/');
#endif
    EXPECT_EQ(count(text, "--gtest_internal_run_death_test"), 0u);
}

// An appended file past 100 MB is moved aside, named after its first line's time, and the run starts a new one
TEST_F(LogTest, LargeLogFileIsMovedAside)
{
    if (!logFile.empty())
        writeLargeLog(logFile);
    EXPECT_EXIT(
    {
        LOG("new run line");
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    const std::string text = logged();
    EXPECT_EQ(count(text, "new run line"), 1u);
    EXPECT_EQ(count(text, "old run line"), 0u);
    const std::filesystem::path aside = fallbackFileOf(logFile);
    ASSERT_FALSE(aside.empty());
    EXPECT_EQ(aside.filename(), std::filesystem::path(logFile).stem().string() + "-20260102-030405.txt");
    EXPECT_GT(std::filesystem::file_size(aside), 100u * 1024 * 1024);
    std::filesystem::remove(aside);
}

#ifndef _WIN32
// The environment's string, not a copy: exit() destroys statics while the logger thread still opens the file
static std::atomic<const char*> gLogToMoveAside {nullptr};

// As another process would do between this one's open and its lock: move the large log aside and hold a new one
static void moveAsideUnderTheOpener(LogTransition transition)
{
    if (transition != LogTransition::SyncLogLocking)
        return;
    const char* const logToMoveAside = gLogToMoveAside.exchange(nullptr);
    if (logToMoveAside == nullptr)
        return;
    const std::filesystem::path name = logToMoveAside;
    const std::filesystem::path aside = name.parent_path() / (name.stem().string() + "-20260102-030405.txt");
    static_cast<void>(link(name.c_str(), aside.c_str()));
    static_cast<void>(unlink(name.c_str()));
    const int other = open(name.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    static_cast<void>(flock(other, LOCK_EX));
    static const char line[] = "the other process's line\n";
    static_cast<void>(write(other, line, sizeof(line) - 1));
}

// The name moves on to a new file between the open and the lock: the run writes neither the archive nor the new file
TEST_F(LogTest, LogMovedAsideBetweenOpenAndLock)
{
    if (!logFile.empty())
        writeLargeLog(logFile);
    EXPECT_EXIT(
    {
        gLogToMoveAside = getenv("LOG_FILE");
        setLogTransitionHook(moveAsideUnderTheOpener);
        LOG("run after the move");
        exit(0);
    },
    testing::ExitedWithCode(0), "is in use by another process");
    const std::filesystem::path log(logFile);
    const std::filesystem::path aside = log.parent_path() / (log.stem().string() + "-20260102-030405.txt");
    EXPECT_EQ(std::filesystem::file_size(aside), 100u * 1024 * 1024 + 1);
    std::filesystem::remove(aside);
    EXPECT_EQ(logged(), "the other process's line\n");
    const std::filesystem::path own = datedPidFileOf(log);
    ASSERT_FALSE(own.empty());
    std::string content;
    readTestFile(own.string(), content);
    remove(own.string().c_str());
    EXPECT_EQ(count(content, "run after the move\n"), 1u);
}
#endif

// LogEarlyInit.cpp logs from an init_seg(lib) initializer when this variable is set
TEST_F(LogTest, LogFromEarlyInitializer)
{
    setenv("LOG_EARLY_INIT_TEST", "1", 1);
    EXPECT_EXIT(
    {
        exit(0);
    },
    testing::ExitedWithCode(0), "");
    unsetenv("LOG_EARLY_INIT_TEST");
    EXPECT_EQ(count(logged(), "logged from an early initializer"), 1u);
}

struct CapturedLine
{
    uint32_t threadId;
    std::string threadName;
    std::string message;
};

static std::mutex gCapturedMutex;
static std::vector<CapturedLine> gCaptured;
static const std::string_view kHoldTheCheck = "hold the check";
static const std::string_view kStraddlingRecord = "straddling record";
static const std::string_view kHoldTheLoggerThread = "hold the logger thread";

// Inside the logger thread's check of pending counts: the late repeats stop, then the check is held half a second
static void stopLateRepeatsAndHoldTheCheck()
{
    gStep.store(2);
    while (gStep.load() == 2)
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
}

// The logger thread has read the line's record and checks no pending count until released
static void holdTheLoggerThread()
{
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
}

// Called on the logger thread for its lines
static void captureLine(uint32_t threadId, std::string_view threadName, std::string_view message)
{
    {
        std::lock_guard lock(gCapturedMutex);
        gCaptured.push_back({threadId, std::string(threadName), std::string(message)});
    }
    // Written late, the count ends in " (the last N s earlier)"
    if (message.starts_with("repeated 1 time: hold the check"))
        stopLateRepeatsAndHoldTheCheck();
    if (message == kStraddlingRecord || message == kHoldTheLoggerThread)
        holdTheLoggerThread();
    if (message.starts_with("slow line "))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// In this process, lines taken from the sink; each case logs from a new thread, so nothing earlier counts as a repeat
class LogLinesTest : public testing::Test
{
protected:
    void SetUp() override
    {
        std::lock_guard lock(gCapturedMutex);
        gCaptured.clear();
        setLogLineHook(captureLine);
        setRepeatCountDelays(100, 1000);
    }

    void TearDown() override
    {
        setRepeatCountDelays(kRepeatCountQuietMs, kRepeatCountMaxMs);
        setLogLineHook(nullptr);
    }

    // What body logged on a new thread, once the thread ended and LOG() returned
    static std::vector<std::string> linesOf(void (*body)())
    {
        uint32_t id = 0;
        std::thread thread([&id, body] { id = logThreadId(); body(); });
        thread.join();
        LOG();
        std::vector<std::string> lines;
        std::lock_guard lock(gCapturedMutex);
        for (const CapturedLine& line : gCaptured)
        {
            if (line.threadId == id)
                lines.push_back(line.message);
        }
        return lines;
    }
};

static void logPrintf()
{
    LOG("int %d %i %u %x %X %o %c", -5, 6, 7u, 255, 255, 8, 'z');
    LOG("long %ld %lu %lld %llu %zu", -9L, 10UL, -11LL, 12ULL, sizeof(int));
    LOG("float %f %.2f %e %g [%10.3f] [%-8.1f]", 1.5, 2.25, 3e10, 0.0001, 4.5, 5.5);
    LOG("string %s [%10s] [%-6s] [%.3s] [%.*s] [%s]", "text", "right", "left", "abcdef", 2, "xyz", "");
    LOG("percent 100%% %s", "done");
}

TEST_F(LogLinesTest, PrintfConversions)
{
    const std::vector<std::string> expected {
        fmtStr("int %d %i %u %x %X %o %c", -5, 6, 7u, 255, 255, 8, 'z'),
        fmtStr("long %ld %lu %lld %llu %zu", -9L, 10UL, -11LL, 12ULL, sizeof(int)),
        fmtStr("float %f %.2f %e %g [%10.3f] [%-8.1f]", 1.5, 2.25, 3e10, 0.0001, 4.5, 5.5),
        "string text [     right] [left  ] [abc] [xy] []",
        "percent 100% done",
    };
    EXPECT_EQ(linesOf(logPrintf), expected);
}

static void logFmt()
{
    LOG("fmt {} {} {} {}", 1, -2LL, 3.5, true);
    const char* pointer = "pointer";
    LOG("strings {} {} {} {}", "literal", std::string("string"), std::string_view("view"), pointer);
    LOG("spec [{:>6}] [{:<6}] [{:^7}] [{:08.3f}] [{:#x}]", 42, "ab", "mid", 3.14159, 255);
    LOG("escaped {{}} {}", 7);
}

static void logBadWidths()
{
    LOG("{:{}}", 42, -1);
    LOG("{:{}}", Counter {7}, -1);
    LOG("after the bad widths");
}

TEST_F(LogLinesTest, FormatErrorIsWrittenAsTheLine)
{
    const std::vector<std::string> lines = linesOf(logBadWidths);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_TRUE(lines[0].starts_with("bad format \"{:{}}\": ")) << lines[0];
    EXPECT_TRUE(lines[1].starts_with("bad format \"{:{}}\": ")) << lines[1];
    EXPECT_EQ(lines[2], "after the bad widths");
}

TEST_F(LogLinesTest, FmtFields)
{
    const std::vector<std::string> expected {
        std::format("fmt {} {} {} {}", 1, -2LL, 3.5, true),
        "strings literal string view pointer",
        std::format("spec [{:>6}] [{:<6}] [{:^7}] [{:08.3f}] [{:#x}]", 42, "ab", "mid", 3.14159, 255),
        "escaped {} 7",
    };
    EXPECT_EQ(linesOf(logFmt), expected);
}

static void logText()
{
    LOG("plain text");
    LOG("");
    LOG("literal with {} and 100%");
    LOG(std::string("std::string text"));
    LOG(std::string_view("string_view text"));
    LLOG("llog %d %s", 5, "printf");
    LLOG(std::string_view("llog text"));
    const int x = 3;
    const int y = 4;
    LOG_ARGS(x, y);
}

TEST_F(LogLinesTest, TextLines)
{
    const std::vector<std::string> expected {
        "plain text", "literal with {} and 100%", "std::string text", "string_view text", "llog 5 printf", "llog text",
        "x:3, y:4",
    };
    EXPECT_EQ(linesOf(logText), expected);
}

static void logLiteralArguments()
{
    LOG("literal argument %s", "kept by its address");
    LOG("literal argument {}", "in a fmt format");
    char local[] = "a local array";
    LOG("local argument %s", local);
    // A record that kept the array's address would show this later
    local[0] = 'X';
    const char* pointer = "a pointer";
    LOG("chosen argument %s", local[0] == 'X' ? "a literal" : pointer);
}

TEST_F(LogLinesTest, LiteralArguments)
{
    const std::vector<std::string> expected {
        "literal argument kept by its address", "literal argument in a fmt format", "local argument a local array",
        "chosen argument a literal",
    };
    EXPECT_EQ(linesOf(logLiteralArguments), expected);
}

static void logEmbeddedNuls()
{
    const char array[] = "a\0b";
    const char* pointer = "a\0b";
    LOG("printf [%s]", "a\0b");
    LOG("printf [%s]", array);
    LOG("printf [%s]", pointer);
    LOG("fmt [{}]", "a\0b");
    LOG("fmt [{}]", array);
    LOG("fmt [{}]", pointer);
    LOG("a\0b");
    LOG(array);
    LOG("fmt [{}]", std::string_view("a\0b", 3));
    LOG("after the last");
}

// A literal ends at its first NUL however it is spelled; only a std::string_view keeps its own length
TEST_F(LogLinesTest, EmbeddedNuls)
{
    const std::vector<std::string> expected {
        "printf [a]", "printf [a]", "printf [a]", "fmt [a]", "fmt [a]", "fmt [a]", "a", "a",
        std::string("fmt [a\0b]", 9), "after the last",
    };
    EXPECT_EQ(linesOf(logEmbeddedNuls), expected);
}

// Spelled with a literal first, each argument is a local array; the logger thread formats it after the array changed
static void logArraysSpelledLikeLiterals()
{
    LOG(kHoldTheLoggerThread);
    while (gStep.load() != 1)
        std::this_thread::yield();
    const std::string side = "BUY";
    char label[] = "BUY";
    static_assert(std::is_same_v<decltype(("BUY" == side ? label : "N/A")), const char (&)[4]>);
    LOG("printf %s", "BUY" == side ? label : "N/A");
    LOG("fmt {}", "BUY" == side ? label : "N/A");
    LOG("raw {} {}", R"(", "fake", )", label);
    LOG("BUY" == side ? label : "N/A");
    LLOG("llog %s", "BUY" == side ? label : "N/A");
    LLOG("BUY" == side ? label : "N/A");
    label[0] = 'X';
    gStep.store(2);
}

TEST_F(LogLinesTest, ArraysSpelledLikeLiteralsAreCopied)
{
    gStep.store(0);
    const std::vector<std::string> expected {
        std::string(kHoldTheLoggerThread), "printf BUY", "fmt BUY", R"(raw ", "fake",  BUY)", "BUY", "llog BUY", "BUY",
    };
    EXPECT_EQ(linesOf(logArraysSpelledLikeLiterals), expected);
}

TEST_F(LogLinesTest, LlogWritesWhatLogWrites)
{
    EXPECT_EQ(linesOf(logStatementsWithLog), kStatementLines);
    EXPECT_EQ(linesOf(logStatementsWithLlog), kStatementLines);
}

static void logChangedArguments()
{
    char buffer[16] = "before";
    std::string text = "before";
    Counter counter {1};
    LOG("buffer %s", buffer);
    LOG(buffer);
    LOG("string {}", text);
    LOG("counter {}", counter);
    memcpy(buffer, "after", 6);
    text = "after";
    counter.value = 2;
}

TEST_F(LogLinesTest, ArgumentsAreTakenAtTheCall)
{
    const std::vector<std::string> expected {"buffer before", "before", "string before", "counter 1"};
    EXPECT_EQ(linesOf(logChangedArguments), expected);
}

static void logRepeats()
{
    for (int i = 0; i < 5; ++i)
        LOG("same %d", 1);
    LOG("other");
    LOG("same %d", 1);
    LOG("same %d", 1);
}

TEST_F(LogLinesTest, RepeatsAreCounted)
{
    const std::vector<std::string> expected {
        "same 1", "repeated 4 times: same 1", "other", "same 1", "repeated 1 time: same 1",
    };
    EXPECT_EQ(linesOf(logRepeats), expected);
}

static void logLookAlikes()
{
    LOG("n %d", 1);
    LOG("n %d", 2);
    LOG("s %s", "a");
    LOG("s %s", "ab");
    LOG("{}", 0.0);
    LOG("{}", -0.0);
    LOG("{}", std::numeric_limits<double>::quiet_NaN());
    LOG("{}", std::numeric_limits<double>::quiet_NaN());
}

TEST_F(LogLinesTest, OnlyEqualArgumentsRepeat)
{
    const std::vector<std::string> expected {
        "n 1", "n 2", "s a", "s ab", "0", "-0", std::format("{}", std::numeric_limits<double>::quiet_NaN()),
        std::format("repeated 1 time: {}", std::numeric_limits<double>::quiet_NaN()),
    };
    EXPECT_EQ(linesOf(logLookAlikes), expected);
}

static void logRepeatedText()
{
    for (int i = 0; i < 3; ++i)
        LOG(std::string("text line"));
    for (int i = 0; i < 2; ++i)
        LLOG("llog %d", 1);
    for (int i = 0; i < 2; ++i)
        LOG("counter {}", Counter {7});
    for (int i = 0; i < 3; ++i)
        LOG("literal line");
}

TEST_F(LogLinesTest, TextRepeatsAreCounted)
{
    const std::vector<std::string> expected {
        "text line", "repeated 2 times: text line", "llog 1", "repeated 1 time: llog 1",
        "counter 7", "repeated 1 time: counter 7", "literal line", "repeated 2 times: literal line",
    };
    EXPECT_EQ(linesOf(logRepeatedText), expected);
}

static void logRepeatText()
{
    LOG("repeated 5 times: a");
    LOG("a");
    LOG("a");
}

TEST_F(LogLinesTest, RepeatTextFromTheCallerIsLogged)
{
    const std::vector<std::string> expected {
        "repeated 5 times: a", "a", "repeated 1 time: a",
    };
    EXPECT_EQ(linesOf(logRepeatText), expected);
}

static const size_t kHugeSize = 3 * 1024 * 1024;

static void logHugeString()
{
    LOG("before the huge line");
    LOG("{}", std::string(kHugeSize, 'x'));
    LOG("after the huge line");
}

TEST_F(LogLinesTest, LineBiggerThanTheQueue)
{
    const std::vector<std::string> lines = linesOf(logHugeString);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "before the huge line");
    EXPECT_EQ(lines[1], std::string(kHugeSize, 'x'));
    EXPECT_EQ(lines[2], "after the huge line");
}

static const size_t kMovingSize = 1024 * 1024;

static void repeatBeforeQueueMoves()
{
    for (int i = 0; i < 3; ++i)
        LOG("line before the move");
    LOG("{}", std::string(kMovingSize, 'm')); // bigger than the queue's first buffer, so Quill moves it to a new one
}

TEST_F(LogLinesTest, CountNamesItsLineAfterTheQueueMoves)
{
    const std::vector<std::string> lines = linesOf(repeatBeforeQueueMoves);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "line before the move");
    EXPECT_EQ(lines[1], "repeated 2 times: line before the move");
    EXPECT_EQ(lines[2], std::string(kMovingSize, 'm'));
}

// A copied text's record: header, length, the text and its NUL
static size_t textRecordSize(size_t length)
{
    return logdetail::kRecordHeader + sizeof(uint32_t) + length + 1;
}

// From the thread's next record to where a record starts whose tag begins at the ring's end
static size_t bytesToTheStraddle()
{
    const logdetail::Writer& writer = *logdetail::tWriter;
    return (writer.mask + 1 - sizeof(uint64_t) - (writer.position & writer.mask)) & writer.mask;
}

static std::vector<std::string> gExpectedLines;

static void logExpected(std::string text)
{
    LOG(text);
    gExpectedLines.push_back(std::move(text));
}

// Each LOG() empties the queue, so every record lands where the thread's position says
static void overwriteTheStraddlingRecord()
{
    gExpectedLines.clear();
    logExpected("before the straddling record");
    LOG();
    if (bytesToTheStraddle() < textRecordSize(1))
    {
        logExpected(std::string(64, 's'));
        LOG();
    }
    logExpected(std::string(bytesToTheStraddle() - textRecordSize(0), 'f'));
    LOG();
    LOG(kStraddlingRecord);
    LOG(kStraddlingRecord);
    gExpectedLines.emplace_back(kStraddlingRecord);
    gExpectedLines.push_back("repeated 1 time: " + std::string(kStraddlingRecord));
    while (gStep.load() != 1)
        std::this_thread::yield();
    // As big as the ring: the logger thread has read the repeated record, so this lands right after it in the same ring
    logExpected(std::string(logdetail::tWriter->mask + 1 - textRecordSize(0), 'x'));
    gStep.store(2);
}

TEST_F(LogLinesTest, CountNamesItsLineAfterTheRingOverwritesIt)
{
    gStep.store(0);
    const std::vector<std::string> lines = linesOf(overwriteTheStraddlingRecord);
    EXPECT_EQ(lines, gExpectedLines);
}

TEST_F(LogLinesTest, FlushWritesLinesOfRunningThreads)
{
    gStep.store(0);
    std::thread thread(logThenWait);
    while (gStep.load() != 1)
        std::this_thread::yield();
    LOG();
    size_t written = 0;
    size_t repeats = 0;
    {
        std::lock_guard lock(gCapturedMutex);
        for (const CapturedLine& line : gCaptured)
        {
            written += line.message.starts_with("waiting thread line ") ? 1 : 0;
            repeats += repeatsIn(line.message, "waiting thread repeat");
        }
    }
    gStep.store(2);
    thread.join();
    EXPECT_EQ(written, 1000u);
    EXPECT_EQ(repeats, 2u);
}

// A pause of the logging thread can split one run into several counts
static size_t capturedRepeats(std::string_view line)
{
    std::lock_guard lock(gCapturedMutex);
    size_t total = 0;
    for (const CapturedLine& captured : gCaptured)
        total += repeatsIn(captured.message, line);
    return total;
}

// A wait for the logger thread fails its test after this instead of hanging it
constexpr auto kLoggerThreadWait = std::chrono::seconds(30);

static bool waitForLoggerThreadRepeats(std::string_view line, size_t repeats)
{
    const auto until = std::chrono::steady_clock::now() + kLoggerThreadWait;
    while (capturedRepeats(line) < repeats)
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

static void repeatThenWait()
{
    for (int i = 0; i < 3; ++i)
        LOG("idle repeat");
    gStep.store(1);
    while (gStep.load() != 2)
        std::this_thread::yield();
}

TEST_F(LogLinesTest, LoggerThreadWritesAnIdleCount)
{
    gStep.store(0);
    std::thread thread(repeatThenWait);
    while (gStep.load() != 1)
        std::this_thread::yield();
    EXPECT_TRUE(waitForLoggerThreadRepeats("idle repeat", 2));
    gStep.store(2);
    thread.join();
    LOG();
    EXPECT_EQ(capturedRepeats("idle repeat"), 2u);
}

static void logSlowLines()
{
    for (int i = 0; i < 30; ++i)
        LOG("slow line %d", i);
}

static void repeatThenWaitForTheCount()
{
    LOG("repeat behind slow lines");
    LOG("repeat behind slow lines");
    while (gStep.load() != 2)
        std::this_thread::yield();
}

// The logger thread writes one slow line per poll, so the repeated line waits behind them after leaving its queue
TEST_F(LogLinesTest, IdleCountFollowsALineThatWaitsToBeWritten)
{
    gStep.store(0);
    std::thread slow(logSlowLines);
    slow.join();
    uint32_t id = 0;
    std::thread repeater([&id] { id = logThreadId(); repeatThenWaitForTheCount(); });
    EXPECT_TRUE(waitForLoggerThreadRepeats("repeat behind slow lines", 1));
    gStep.store(2);
    repeater.join();
    LOG();
    std::vector<std::string> lines;
    {
        std::lock_guard lock(gCapturedMutex);
        for (const CapturedLine& line : gCaptured)
        {
            if (line.threadId == id)
                lines.push_back(line.message);
        }
    }
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "repeat behind slow lines");
    EXPECT_EQ(repeatsIn(lines[1], "repeat behind slow lines"), 1u);
}

static void repeatUntilTold()
{
    LOG("running repeat");
    while (gStep.load() != 2)
        LOG("running repeat");
}

TEST_F(LogLinesTest, LoggerThreadWritesARunningCountEachSecond)
{
    gStep.store(0);
    std::thread thread(repeatUntilTold);
    const auto until = std::chrono::steady_clock::now() + kLoggerThreadWait;
    bool written = false;
    while (!written && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(gCapturedMutex);
        for (const CapturedLine& line : gCaptured)
            written = written || line.message.ends_with(" times: running repeat");
    }
    EXPECT_TRUE(written);
    gStep.store(2);
    thread.join();
}

static void repeatAcrossRename()
{
    setThreadName("BeforeRepeats");
    LOG("renamed repeat");
    LOG("renamed repeat");
    setThreadName("AfterRepeats");
    LOG("renamed repeat");
}

// A rename keeps the run going; the count goes out under the new name
TEST_F(LogLinesTest, RepeatsCountOnAcrossARename)
{
    const std::vector<std::string> expected {"renamed repeat", "repeated 2 times: renamed repeat"};
    EXPECT_EQ(linesOf(repeatAcrossRename), expected);
    std::lock_guard lock(gCapturedMutex);
    for (const CapturedLine& line : gCaptured)
    {
        if (line.message == "repeated 2 times: renamed repeat")
        {
            EXPECT_EQ(line.threadName, "AfterRepeats");
        }
    }
}

static void repeatUntilTheCheckStopsIt()
{
    LOG("late repeat");
    gStep.store(1);
    while (gStep.load() == 1)
        LOG("late repeat");
    int stopped = 2;
    gStep.compare_exchange_strong(stopped, 3);
    while (gStep.load() != 4)
        std::this_thread::yield();
}

static void repeatHoldTheCheck()
{
    LOG(kHoldTheCheck);
    LOG(kHoldTheCheck);
    while (gStep.load() != 4)
        std::this_thread::yield();
}

// The check that writes kHoldTheCheck's count stops the late repeats and is held; the next one writes their count late
TEST_F(LogLinesTest, LateCountSaysWhenItsLastRepeatWas)
{
    // A count overdue at the held check would be written on time, without the words awaited here
    setRepeatCountDelays(100, kRepeatCountMaxMs);
    gStep.store(0);
    std::thread repeater(repeatUntilTheCheckStopsIt);
    while (gStep.load() == 0)
        std::this_thread::yield();
    // Its queue is newer, so the check reaches it before the repeater's
    std::thread holder(repeatHoldTheCheck);
    const auto until = std::chrono::steady_clock::now() + kLoggerThreadWait;
    bool late = false;
    while (!late && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        std::lock_guard lock(gCapturedMutex);
        for (const CapturedLine& line : gCaptured)
        {
            const bool suffixed = line.message.find(": late repeat (the last ") != std::string::npos;
            late = late || (suffixed && line.message.ends_with(" s earlier)"));
        }
    }
    EXPECT_TRUE(late);
    gStep.store(4);
    repeater.join();
    holder.join();
}
