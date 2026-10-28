#include "log.h"
#include "AsyncLog.h"
#include "SyncLog.h"
#include "sysutil.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <iterator>
#include <string>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#include <crtdbg.h>
#endif

#undef LOG

// what: the crash, for the line naming the LOG whose formatting caused it
static void flushForCrash(const char* what)
{
    // A crash inside the line writer keeps the log's lock, so nothing can be written any more
    if (holdsLogLock())
        return;
    setCrashFlushing(true);
    writeLineBeingFormatted(what);
    static constinit std::atomic_flag flushing;
    if (!flushing.test_and_set(std::memory_order_acquire))
    {
        flushAsyncLogForCrash();
        flushing.clear(std::memory_order_release);
    }
    setCrashFlushing(false);
}

#if defined(_WIN32) && defined(_MSC_VER)
[[maybe_unused]] static int crtDbgHook(int nReportType, char* szMsg, int* pnRet)
{
    UNUSED(nReportType); UNUSED(pnRet);
    flushForCrash(szMsg != nullptr ? szMsg : "a CRT report");
    return FALSE; // error is unhandled. Continue with other handlers
}
#endif

static std::terminate_handler oldTerminate = nullptr;

// The exception std::terminate was called for, if any
static std::string currentExceptionText()
{
    const std::exception_ptr current = std::current_exception();
    if (current == nullptr)
        return "std::terminate";
    try
    {
        std::rethrow_exception(current);
    }
    catch (const std::exception& error)
    {
        return error.what();
    }
    catch (...)
    {
        return "an exception that is not a std::exception";
    }
}

#ifdef _WIN32
static LPTOP_LEVEL_EXCEPTION_FILTER oldSeh = nullptr;
static void (*oldAbort)(int) = nullptr;

static std::string sehText(DWORD code)
{
    if (code == EXCEPTION_ACCESS_VIOLATION)
        return "access violation";
    if (code == EXCEPTION_STACK_OVERFLOW)
        return "stack overflow";
    return std::format("exception 0x{:08X}", code);
}

// After the flush, a handler installed before ours decides, as it would have without us: it may mend the crash
static LONG WINAPI WindowsSehHandler(EXCEPTION_POINTERS* exception)
{
    flushForCrash(sehText(exception->ExceptionRecord->ExceptionCode).c_str());
    return oldSeh != nullptr ? oldSeh(exception) : EXCEPTION_CONTINUE_SEARCH;
}

static void SignalWrapper(int sig)
{
    flushForCrash("abort");
    if (oldAbort == SIG_IGN)
        return;
    if (oldAbort != SIG_DFL && oldAbort != nullptr)
    {
        oldAbort(sig);
        return;
    }
    // As abort ends with no handler: after one, it exits through _exit, freeing stdio under the logger thread's writes
#ifdef _MSC_VER
    if ((_set_abort_behavior(0, 0) & _CALL_REPORTFAULT) != 0)
        __fastfail(FAST_FAIL_FATAL_APP_EXIT);
#endif
    TerminateProcess(GetCurrentProcess(), 3);
}
#else
static constexpr int kCrashSignals[] = {SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS, SIGTRAP};
static struct sigaction oldActions[std::size(kCrashSignals)];

// By name, the codes differ between systems; macOS gives kill and raise 0, not its positive SI_USER
static bool sentBySoftware(const siginfo_t& info)
{
#ifdef SI_TKILL
    if (info.si_code == SI_TKILL)
        return true;
#endif
    return info.si_code == 0 || info.si_code == SI_USER || info.si_code == SI_QUEUE;
}

// Not strsignal, which may translate and allocate: the crash may be inside the allocator
static const char* signalText(int sig)
{
    switch (sig)
    {
    case SIGSEGV:
        return "segmentation fault";
    case SIGABRT:
        return "abort";
    case SIGFPE:
        return "floating-point exception";
    case SIGILL:
        return "illegal instruction";
    case SIGBUS:
        return "bus error";
    case SIGTRAP:
        return "trap";
    default:
        return "a signal";
    }
}

static void PosixSignalHandler(int sig, siginfo_t* info, void* context)
{
    flushForCrash(signalText(sig));
    for (size_t i = 0; i < std::size(kCrashSignals); ++i)
    {
        if (kCrashSignals[i] != sig)
            continue;
        const struct sigaction& old = oldActions[i];
        if ((old.sa_flags & SA_SIGINFO) != 0 && old.sa_sigaction != nullptr)
        {
            old.sa_sigaction(sig, info, context);
            return;
        }
        if ((old.sa_flags & SA_SIGINFO) == 0 && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)
        {
            old.sa_handler(sig);
            return;
        }
        // Ignored before ours: a signal from raise or kill stays ignored; a fault can't be, it would only come back
        if ((old.sa_flags & SA_SIGINFO) == 0 && old.sa_handler == SIG_IGN && sentBySoftware(*info))
            return;
    }
    // None: the default action, a core dump or the debugger
    struct sigaction sa = {};
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, nullptr);
    raise(sig);
}
#endif

static void TerminateHandler()
{
    flushForCrash(currentExceptionText().c_str());
    auto handler = oldTerminate;
    oldTerminate = nullptr;
    if (handler)
        handler();
    else
        std::abort();
}

void setCrashHooks()
{
    oldTerminate = std::set_terminate(TerminateHandler); // C++ Standard
#ifdef _WIN32
    oldSeh = SetUnhandledExceptionFilter(WindowsSehHandler); // Windows SEH (Access Violations, Stack Overflow)
    oldAbort = signal(SIGABRT, SignalWrapper); // Windows Signal (abort() calls)
#if defined(_DEBUG) && defined(_MSC_VER)
    _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, crtDbgHook); // Debug-only CRT Reporting Hook
#endif
#else
    // POSIX sigaction
    struct sigaction sa = {};
    sa.sa_sigaction = PosixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    for (int i = 0; i < std::ssize(kCrashSignals); ++i)
        sigaction(kCrashSignals[i], &sa, &oldActions[i]);
#endif
}

void setLoggerThreadCrashHooks()
{
#ifdef _MSC_VER
    std::set_terminate(TerminateHandler);
#endif
}

void unsetCrashHooks()
{
    std::set_terminate(oldTerminate); // Restore C++ Terminate
    oldTerminate = nullptr;
#ifdef _WIN32
    SetUnhandledExceptionFilter(oldSeh); // Restore SEH
    oldSeh = nullptr;
    signal(SIGABRT, oldAbort); // Restore Abort Signal
    oldAbort = nullptr;
#if defined(_DEBUG) && defined(_MSC_VER)
    _CrtSetReportHook2(_CRT_RPTHOOK_REMOVE, crtDbgHook); // Remove Debug Hook
#endif
#else
    for (int i = 0; i < std::ssize(kCrashSignals); ++i)
        sigaction(kCrashSignals[i], &oldActions[i], nullptr); // Restore POSIX Signals
#endif
}

void logdetail::logFormatted(std::string_view format, std::format_args args)
{
    // Dropped, so not formatted: at exit the CRT's heap can be gone before a detached thread's last LOG
    if (syncLinesDropped())
        return;
    std::string text;
    formatFmt(text, format, args);
    submitText(text);
}

void LOG(std::string_view text)
{
    if (text.empty())
        flushAsyncLog();
    else
        logdetail::submitText(text);
}
