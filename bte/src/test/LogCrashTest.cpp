#include <gtest/gtest.h>
#include "format.h"
#include "sysutil.h"
#include "TestFile.h"
#include "SyncLog.h"
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

// Death tests: the crash hooks write the lines queued before a crash; each child logs to the LOG_FILE its parent set

class LogCrashTest : public testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        LOG("LogCrashTest suite starting");
        LOG(); // flush: ensures worker thread is running and log file is open
    }

    // Call before EXPECT_EXIT to set LOG_FILE to a unique name for this test.
    static void setupLogFile()
    {
        if (!GTEST_FLAG_GET(internal_run_death_test).empty())
            return; // the subprocess logs to the file its parent named
        const char* given = getenv("LOG_FILE");
        givenLogFile = given != nullptr ? given : "";
        const std::string name = fmtStr("crash-log-%u-%llu.txt", currentProcessId(), (unsigned long long)timeMicro());
        setenv("LOG_FILE", (std::filesystem::temp_directory_path() / name).string().c_str(), 1);
    }

    static bool logFileContains(const char* marker)
    {
        std::string content;
        if (const char* logFile = getenv("LOG_FILE"))
        {
            readTestFile(logFile, content);
            remove(logFile);
        }
        // The LOG_FILE the test program was given, if any, so the next test names a fresh file again
        if (givenLogFile.empty())
            unsetenv("LOG_FILE");
        else
            setenv("LOG_FILE", givenLogFile.c_str(), 1);
        return content.find(marker) != std::string::npos;
    }

    static inline std::string givenLogFile;

    // Distinct fillers (a repeat would only be counted) keep the marker queued when the crash occurs
    static void logFillers()
    {
        LOG("...");
        for (int i = 0; i < 20; ++i)
            LOG("... %d", i);
    }
};

TEST_F(LogCrashTest, FlushOnAbort)
{
    setupLogFile();
    // abort() -> SIGABRT -> PosixSignalHandler (POSIX) / SignalWrapper (Windows) -> flushForCrash()
    EXPECT_EXIT(
    {
        logFillers();
        LOG("LOG_CRASH_MARKER_ABORT");
        std::abort();
    },
    [](int) { return true; },
    "");
    EXPECT_TRUE(logFileContains("LOG_CRASH_MARKER_ABORT"));
}

TEST_F(LogCrashTest, FlushOnNullWrite)
{
    setupLogFile();
    // Null pointer write -> SIGSEGV / PosixSignalHandler (POSIX) or access-violation / WindowsSehHandler (Windows)
    EXPECT_EXIT(
    {
        logFillers();
        LOG("LOG_CRASH_MARKER_SEGV");
        *(volatile int*)nullptr = 0;
    },
    [](int) { return true; },
    "");
    EXPECT_TRUE(logFileContains("LOG_CRASH_MARKER_SEGV"));
}

TEST_F(LogCrashTest, CountOfTheCrashingThread)
{
    setupLogFile();
    EXPECT_EXIT(
    {
        logFillers();
        LOG("LOG_CRASH_MARKER_REPEAT");
        LOG("LOG_CRASH_MARKER_REPEAT");
        std::abort();
    },
    [](int) { return true; },
    "");
    EXPECT_TRUE(logFileContains("repeated 1 time: LOG_CRASH_MARKER_REPEAT"));
}

static void crashOnMarker(uint32_t, std::string_view, std::string_view message)
{
    if (message == "LOG_CRASH_MARKER_WRITER")
        *(volatile int*)nullptr = 0;
}

TEST_F(LogCrashTest, CrashInsideTheLineWriterEndsTheProcess)
{
    setupLogFile();
    EXPECT_EXIT(
    {
        setLogLineHook(crashOnMarker);
        LOG("LOG_CRASH_MARKER_WRITER");
        LOG();
        exit(0);
    },
    [](int code) { return code != 0; },
    "");
    EXPECT_TRUE(logFileContains("LogCrashTest suite starting"));
}

TEST_F(LogCrashTest, FlushOnTerminate)
{
    setupLogFile();
    // std::terminate() -> TerminateHandler -> flushForCrash() -> std::abort()
    EXPECT_EXIT(
    {
        logFillers();
        LOG("LOG_CRASH_MARKER_TERMINATE");
        std::terminate();
    },
    [](int) { return true; },
    "");
    EXPECT_TRUE(logFileContains("LOG_CRASH_MARKER_TERMINATE"));
}
