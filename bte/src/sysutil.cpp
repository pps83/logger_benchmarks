#include "sysutil.h"
#include "format.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#ifndef _WIN32
#include <pthread.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include <sys/types.h>
#endif
#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#endif

// On Cygwin this is Cygwin's own pid, the one its fork and waitpid use, not the Win32 one.
unsigned currentProcessId()
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned>(getpid());
#endif
}

#if defined(_WIN32) || defined(__CYGWIN__)
unsigned currentThreadId()
{
    return GetCurrentThreadId();
}

bool debuggerPresent()
{
    return 0 != IsDebuggerPresent();
}
#else
unsigned currentThreadId()
{
    return static_cast<unsigned>(pthread_self());
}

bool debuggerPresent()
{
#ifdef __APPLE__
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    size_t size = sizeof(info);
    info.kp_proc.p_flag = 0;
    sysctl(mib, sizeof(mib) / sizeof(*mib), &info, &size, NULL, 0);
    return ((info.kp_proc.p_flag & P_TRACED) != 0);
#else
    FILE* fp = fopen("/proc/self/status", "r");
    if (fp == NULL)
        return 0; // or handle error
    char line[256];
    while (fgets(line, sizeof(line), fp))
    {
        if (strncmp(line, "TracerPid:", 10) == 0)
        {
            int tracer_pid;
            sscanf(line + 10, "%d", &tracer_pid);
            fclose(fp);
            return tracer_pid != 0;
        }
    }
    fclose(fp);
    return false;
#endif
}
#endif // defined(_WIN32) || defined(__CYGWIN__)

#ifdef _MSC_VER
static void setDebuggerThreadName(DWORD threadID, const char* threadName)
{
#pragma pack(push,8)
    struct THREADNAME_INFO
    {
        DWORD dwType; // Must be 0x1000.
        LPCSTR szName; // Pointer to name (in user addr space).
        DWORD dwThreadID; // Thread ID (-1=caller thread).
        DWORD dwFlags; // Reserved for future use, must be zero.
    };
#pragma pack(pop)

    const DWORD MS_VC_EXCEPTION = 0x406D1388;

    THREADNAME_INFO info;
    info.dwType = 0x1000;
    info.szName = threadName;
    info.dwThreadID = threadID;
    info.dwFlags = 0;
    MSVC_WARNING_DISABLE(6320 6322)
    __try
    {
        RaiseException(MS_VC_EXCEPTION, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    MSVC_WARNING_RESTORE()
}
#endif

// Set by the logger while it runs, so renames reach it without this file depending on the logger
static constinit std::atomic<void (*)(const char*)> gThreadNameListener {nullptr};

void setThreadNameListener(void (*listener)(const char* threadName))
{
    gThreadNameListener.store(listener, std::memory_order_release);
}

static constinit thread_local char tThreadName[64] = {};

const char* givenThreadName()
{
    return tThreadName;
}

void setThreadName(const char* threadName)
{
    const size_t kept = strnlen(threadName, sizeof(tThreadName) - 1);
    memcpy(tThreadName, threadName, kept);
    tThreadName[kept] = '\0';
#if defined(_WIN32) || defined(__CYGWIN__)
    const size_t maxLen = 128; // avoid dynamic allocations for strings less than 128 chars
    const size_t threadNameLen = strlen(threadName);
    HRESULT hr;
    if (threadNameLen >= maxLen)
    {
        std::wstring wthreadName(threadName, threadName + threadNameLen);
        hr = SetThreadDescription(GetCurrentThread(), wthreadName.c_str()); // supported from Win10
    }
    else
    {
        wchar_t wthreadName[maxLen];
        for (size_t i = 0; i < threadNameLen; ++i)
            wthreadName[i] = threadName[i];
        wthreadName[threadNameLen] = 0;
        hr = SetThreadDescription(GetCurrentThread(), wthreadName); // supported from Win10
    }
#ifdef _MSC_VER
    if (FAILED(hr))
        setDebuggerThreadName(currentThreadId(), threadName); // visible only if called when debugger was attached
#endif
    UNUSED(hr);
#elif defined(__linux__)
    char kernelName[16]; // the kernel keeps 15 characters
    const size_t kernelKept = strnlen(threadName, sizeof(kernelName) - 1);
    memcpy(kernelName, threadName, kernelKept);
    kernelName[kernelKept] = '\0';
    pthread_setname_np(pthread_self(), kernelName);
#elif defined(__APPLE__)
    pthread_setname_np(threadName);
#endif
    if (auto listener = gThreadNameListener.load(std::memory_order_acquire))
        listener(threadName);
}

#ifdef _WIN32
// Return 0 on success, -1 on error
int setenv(const char* name, const char* value, int overwrite)
{
    if (name == NULL || name[0] == '\0' || strchr(name, '=') != NULL)
    {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite)
    {
        size_t envsize = 0;
        if (getenv_s(&envsize, NULL, 0, name) == 0 && envsize > 0) // Check if the variable already exists
            return 0;
    }
    return (_putenv_s(name, value) == 0) ? 0 : -1; // _putenv_s returns 0 on success
}

// Return 0 on success, -1 on error
int unsetenv(const char* name)
{
    if (!name || !name[0] || strchr(name, '=') != NULL)
    {
        errno = EINVAL;
        return -1;
    }
    return (_putenv_s(name, "") == 0) ? 0 : -1; // In Windows, setting a variable to an empty string removes it
}
#endif
