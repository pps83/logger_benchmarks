#pragma once
#include "format.h"

void logSync(long long timeTick, std::string_view msg);
void flushSyncLog();
void flushSyncLogIfWritten(); // the idle flush: nothing to do when no line was written since the last flush

extern bool AsyncLogSilent; // if set, logger discards logged messages
static const size_t kLogPrefixSize = 64;
static const size_t kThreadNameWidth = 20;

// The calling thread's id, as Quill records it
uint32_t logThreadId();

// The calling thread's name as the OS keeps it, cut to the log's name column
void readThreadName(char (&name)[kThreadNameWidth + 1]);

// line: kLogPrefixSize bytes for the prefix, the message and '\n'; shared by the logger thread and synchronous writes
void writeLogLine(std::string& line, double epochSeconds, uint32_t threadId, std::string_view threadName, bool flush);

// True inside writeLogLine, which a crash there never leaves
bool holdsLogLock();

// While set, the thread's waits for the log's lock end after a second and it leaves stdio alone, so a crash goes on
void setCrashFlushing(bool flushing);

// A crash flush stops waiting for lines once none was written for this long
constexpr long long kCrashWaitMs = 10000;
void setCrashWaitMs(long long ms); // tests shorten it
long long crashWaitMs();

// At the async logger's end: from then on only the calling thread, which runs the exit, logs synchronously
void stopSyncLog();

// True on every thread but the one that ran stopSyncLog, once it ran
bool syncLogStopped();

// True once ExitProcess ended the process's other threads where they stood, maybe holding a lock; never on POSIX
bool processExiting();

// While set, every line goes to it instead of the file, stdout and the debugger; it runs under the log's lock
using LogLineHook = void (*)(uint32_t threadId, std::string_view threadName, std::string_view message);
void setLogLineHook(LogLineHook hook);

// The logger's lifetime transitions; log_stress pauses the thread making one while other threads log
enum class LogTransition
{
    SyncLogOpen,    // under the log's lock, before the log file's first open
    SyncLogLocking, // POSIX, a log file opened and not yet locked
    AsyncStart,     // under the queues' lock, before Quill starts
    AsyncRunning,   // Quill's logger thread runs; LOG doesn't see the logger yet
    AsyncPublished, // LOG sees the logger; LLOG's records don't go to it yet
    AsyncFirstRead, // on the logger thread, before its first read of the queues
    AsyncEnd,       // the async logger's end, before anything ended
    AsyncEndWaited, // the queued lines are written; the logger hasn't ended
    AsyncEnded,     // the logger thread stopped; the threads' pending counts not yet written
    SyncLogClose,   // DestructionManager's last object, before the log file closes
    SyncLogClosed,  // the log file closed: from here on nothing is logged
};

// While set, it runs on the thread making each transition
using LogTransitionHook = void (*)(LogTransition transition);
void setLogTransitionHook(LogTransitionHook hook);
void logTransition(LogTransition transition);

// The calling thread's pending synchronous LLOG count; endRun: a different line follows, so an equal LLOG is a new line
void writeSyncRepeats(bool endRun);

// Set by the async logger from its start to its end: LLOG's records and flushes go to it
using LlogRecordSink = void (*)(const unsigned char* record, size_t size);
void setLlogRedirect(LlogRecordSink sink, void (*flush)());

// A pending repeat count is written once it stays the same for quietMs, or maxMs after its first repeat
constexpr long long kRepeatCountQuietMs = 2000;
constexpr long long kRepeatCountMaxMs = 10000;
void setRepeatCountDelays(long long quietMs, long long maxMs); // tests shorten them
long long repeatCountQuietMs();
long long repeatCountMaxMs();
