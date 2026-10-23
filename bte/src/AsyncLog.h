#pragma once
// LOG's background thread (Quill). Without it, LOG writes synchronously.
#include <stddef.h>

// Before the first LOG: the size each thread's queue starts at and the most it grows to (log_stress makes them small)
void setLogQueueCapacity(size_t initial, size_t max);

// Waits until every line logged before it is written, however long that takes; on the logger thread it returns
void flushAsyncLog();

// A crash the program may outlive: writes the lines queued until now and the pending counts; the logger goes on
void flushAsyncLogForCrash();

// In a crash handler: the queued LOG this thread was formatting, as written, with its values and what went wrong
void writeLineBeingFormatted(const char* what);

// Defined in log.cpp
void setCrashHooks();
void unsetCrashHooks();
void setLoggerThreadCrashHooks();
