// The sink takes thread names from bte's own name records, so Quill doesn't read them on each thread's first log
#define QUILL_NO_THREAD_NAME_SUPPORT
#include "AsyncLog.h"
#include "SyncLog.h"
#include "DestructionManager.h"
#include "sysutil.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/GlobalState.h"
#include "quill/sinks/Sink.h"
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace
{

// A burst grows the queue up to 2 MiB; past that the caller waits for the logger thread, no line is dropped
struct QueueOptions : quill::FrontendOptions
{
    static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
    static constexpr size_t initial_queue_capacity = 256u * 1024u;
    static constexpr size_t unbounded_queue_max_capacity = 2u * 1024u * 1024u;
};

constinit size_t gQueueCapacity = QueueOptions::initial_queue_capacity;
constinit size_t gQueueMaxCapacity = QueueOptions::unbounded_queue_max_capacity;

using QuillLogger = quill::LoggerImpl<QueueOptions>;
using Frontend = quill::FrontendImpl<QueueOptions>;
using Producer = quill::detail::QueueProducer<size_t>;

// Distinct tags stop the linker from merging these
constexpr quill::MacroMetadata kLineMetadata {"", "", "{}", "line", quill::LogLevel::Info,
                                               quill::MacroMetadata::Event::Log};
constexpr quill::MacroMetadata kThreadNameMetadata {"", "", "{}", "thread name", quill::LogLevel::Info,
                                                     quill::MacroMetadata::Event::Log};
// A thread's last record, so the logger thread keeps no name of every thread that ever logged
constexpr quill::MacroMetadata kThreadEndMetadata {"", "", "{}", "thread end", quill::LogLevel::Info,
                                                    quill::MacroMetadata::Event::Log};
// A Quill record's header: timestamp, MacroMetadata*, LoggerBase*, decoder (Logger.h, s_header_size)
constexpr size_t kQuillHeaderSize = 4 * sizeof(uint64_t);

using logdetail::siteOf;

// The queued record this thread formats, for the line a crash in its formatting writes
constinit thread_local uint64_t tFormattingTag = 0;
constinit thread_local const unsigned char* tFormattingArgs = nullptr;

// LOG("<format>", <arguments>) failed: <what>; values: <values>; returns where the record's arguments end
const unsigned char* failedLine(uint64_t tag, const unsigned char* args, const char* what, std::string& out)
{
    const logdetail::Site& site = siteOf(tag);
    std::string values;
    const unsigned char* end = site.values(args, values);
    out.clear();
    if (site.text == nullptr)
        out += "a LOG failed";
    else
    {
        out += site.llog ? "LLOG(" : "LOG(";
        logdetail::appendQuoted(out, site.text);
        out += ", ";
        out += site.text + strlen(site.text) + 1;
        out += ") failed";
    }
    out += ": ";
    out += what;
    if (!values.empty())
        out += "; values: " + values;
    return end;
}

#ifndef NDEBUG
// A stop the debugger goes on from, and the logger with it
void stopForDebugger()
{
#ifdef _WIN32
    DebugBreak();
#else
    raise(SIGTRAP);
#endif
}
#endif

// noexcept: a failed line that can't be built (no memory) ends the process; with a debugger, Debug stops here first
const unsigned char* formatterThrew(uint64_t tag, const unsigned char* args, const char* what,
                                    std::string& out) noexcept
{
#ifndef NDEBUG
    if (debuggerPresent())
        stopForDebugger();
#endif
    return failedLine(tag, args, what, out);
}

// The record this thread formats; the outer one again after, as a repeat count formats the repeated record inside it
struct FormattingRecord
{
    const uint64_t outerTag = tFormattingTag;
    const unsigned char* const outerArgs = tFormattingArgs;

    FormattingRecord(uint64_t tag, const unsigned char* args)
    {
        tFormattingTag = tag;
        tFormattingArgs = args;
    }

    ~FormattingRecord()
    {
        tFormattingTag = outerTag;
        tFormattingArgs = outerArgs;
    }

    FormattingRecord(const FormattingRecord&) = delete;
    FormattingRecord& operator=(const FormattingRecord&) = delete;
};

// The record's LOG returned, so no one can catch a formatter's exception: the line says it failed, the logger goes on
const unsigned char* formatQueued(uint64_t tag, const unsigned char* args, std::string& out)
{
    const FormattingRecord formatting(tag, args);
    const unsigned char* end = nullptr;
    try
    {
        end = siteOf(tag).format(args, out);
    }
    catch (const std::exception& error)
    {
        end = formatterThrew(tag, args, error.what(), out);
    }
    catch (...)
    {
        end = formatterThrew(tag, args, "an exception that is not a std::exception", out);
    }
    return end;
}

// "repeated N times: " and the repeated record's line (tag 0: not known); returns where the record's arguments end
const unsigned char* formatRepeatCount(std::string& out, uint64_t count, const unsigned char* record)
{
    uint64_t tag;
    memcpy(&tag, record + sizeof(uint64_t), sizeof(tag));
    const unsigned char* end = record + logdetail::kRecordHeader;
    out.clear();
    if (tag != 0)
        end = formatQueued(tag, end, out);
    out.insert(0, "repeated " + std::to_string(count) + (count > 1 ? " times" : " time") + (tag != 0 ? ": " : ""));
    return end;
}

// A record of the repeated record's layout, with the count where the timestamp is
const unsigned char* formatRepeats(const unsigned char* bytes, std::string& out)
{
    uint64_t count;
    memcpy(&count, bytes, sizeof(count));
    return formatRepeatCount(out, count, bytes);
}

const unsigned char* repeatedValues(const unsigned char* bytes, std::string& out)
{
    uint64_t tag;
    memcpy(&tag, bytes + sizeof(uint64_t), sizeof(tag));
    out.clear();
    const unsigned char* args = bytes + logdetail::kRecordHeader;
    return tag != 0 ? siteOf(tag).values(args, out) : args;
}

constexpr logdetail::Site kRepeatSite {&formatRepeats, &repeatedValues};

// Quill and bte's tsc clock read the same counter here, so the sink converts Quill's raw value with bte's code
#if (defined(_WIN32) || defined(__CYGWIN__) || defined(__linux__)) \
    && (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
constexpr bool kRawTimestamps = true;
#else
constexpr bool kRawTimestamps = false;
#endif

double epochSeconds(uint64_t timestamp)
{
    if constexpr (kRawTimestamps)
        return tsc::ticksToTime(tsc::rawToTicks(static_cast<long long>(timestamp)));
    else
        return static_cast<double>(timestamp) / 1e9;
}

// A crash waits for the queued lines as long as this keeps moving
constinit std::atomic<uint64_t> gLinesWritten {0};
// A crash flush's request for the pending counts, and the last request the logger thread served
constinit std::atomic<uint64_t> gCountsWanted {0};
constinit std::atomic<uint64_t> gCountsWritten {0};

// Writes lines to the log file, stdout and the debugger
class LogSink : public quill::Sink
{
public:
    void write_log(const quill::MacroMetadata* metadata, uint64_t timestamp, std::string_view threadId,
                   std::string_view, const std::string&, std::string_view, quill::LogLevel, std::string_view,
                   std::string_view, const std::vector<std::pair<std::string, std::string>>*,
                   std::string_view message, std::string_view) override
    {
        uint32_t id = 0;
        std::from_chars(threadId.data(), threadId.data() + threadId.size(), id);
        if (metadata == &kThreadNameMetadata)
        {
            threadNames[id] = message;
            return;
        }
        if (metadata == &kThreadEndMetadata)
        {
            threadNames.erase(id);
            return;
        }
        line.resize(kLogPrefixSize);
        line.append(message);
        line += '\n';
        const auto name = threadNames.find(id);
        const std::string_view threadName = name != threadNames.end() ? std::string_view(name->second) : "";
        writeLogLine(line, epochSeconds(timestamp), id, threadName, false);
        gLinesWritten.fetch_add(1, std::memory_order_relaxed);
    }

    void flush_sink() override
    {
        flushSyncLog();
    }

    void flush_sink(quill::SinkFlushReason reason) override
    {
        if (reason == quill::SinkFlushReason::Periodic)
            flushSyncLogIfWritten();
        else
            flushSyncLog();
    }

private:
    std::string line;
    std::unordered_map<uint32_t, std::string> threadNames; // from each thread's queue, ahead of its lines
};

constinit std::atomic<QuillLogger*> gLogger {nullptr};
constinit std::atomic<bool> gLoggerEnded {false};
constinit std::atomic<int> gQuillUsers {0};

// Taken around a Quill call made without gQueuesLock: the logger's end waits for it before it frees Quill's managers
struct QuillUse
{
    QuillUse()
    {
        gQuillUsers.fetch_add(1);
    }

    ~QuillUse()
    {
        gQuillUsers.fetch_sub(1);
    }

    QuillUse(const QuillUse&) = delete;
    QuillUse& operator=(const QuillUse&) = delete;

    // Sequentially consistent with the logger's end: either this sees no logger or the end sees this use
    explicit operator bool() const
    {
        return gLogger.load() != nullptr;
    }
};
constinit thread_local bool tQueueEnded = false; // the thread's later LOGs write synchronously

#ifdef _WIN32
constinit thread_local bool tThreadExiting = false;

// Before the runtime's thread_local destructors (.CRT$XLD), as Windows calls TLS callbacks in their section's order
void NTAPI markThreadExiting(PVOID, DWORD reason, PVOID)
{
    if (reason == DLL_THREAD_DETACH)
        tThreadExiting = true;
}
#endif

// No queue for a thread whose first LOG comes in its exit destructors: on Windows its ThreadExit would never run
bool exitingWithoutQueue()
{
#ifdef _WIN32
    return tThreadExiting && quill::detail::FrontendThreadState::thread_context == nullptr;
#else
    return false;
#endif
}
// Set under gQueuesLock before the logger thread's first read. Until then a thread writes its own queue: a Windows
// thread running its exit destructors holds the loader lock, and the logger thread cannot start before it ends
constinit std::atomic<bool> gLoggerReading {false};

// Held around a thread's queue writes outside LOG and around the logger's end, so neither passes the other
constinit std::atomic_flag gQueuesLock;
constinit thread_local bool tHoldsQueuesLock = false;

// Recursive, so a thread holding it can still write its repeat count
struct QueuesLock
{
    const bool taken = !tHoldsQueuesLock;

    QueuesLock()
    {
        if (!taken)
            return;
        while (gQueuesLock.test_and_set(std::memory_order_acquire))
            std::this_thread::yield();
        tHoldsQueuesLock = true;
    }

    ~QueuesLock()
    {
        if (!taken)
            return;
        tHoldsQueuesLock = false;
        gQueuesLock.clear(std::memory_order_release);
    }
};

// Taken only if free, for the logger thread: a thread holding it may be waiting for the logger thread
struct TriedQueuesLock
{
    const bool taken = !gQueuesLock.test_and_set(std::memory_order_acquire);

    TriedQueuesLock()
    {
        if (taken)
            tHoldsQueuesLock = true;
    }

    ~TriedQueuesLock()
    {
        if (!taken)
            return;
        tHoldsQueuesLock = false;
        gQueuesLock.clear(std::memory_order_release);
    }

    TriedQueuesLock(const TriedQueuesLock&) = delete;
    TriedQueuesLock& operator=(const TriedQueuesLock&) = delete;
};

// A thread with a queue, listed for the logger's end. On the heap: when a runtime skips the thread's destructors, it
// stays listed and readable instead of left in freed TLS
struct ThreadQueue
{
    logdetail::Writer writer;
    ThreadQueue* next;
    uint32_t threadId;
    char threadName[kThreadNameWidth + 1];
    uint64_t writtenRepeats; // of the writer's repeats, those the logger thread or the logger's end wrote for it
    std::vector<unsigned char> lastRecord; // the last record, kept for the repeat check over Quill's next write
    quill::detail::ThreadContext* context; // the logger thread reads its queue
    uint64_t seenRepeats;                  // the logger thread's view of a pending count, in its clock's ticks:
    long long pendingSince;                //   when it first saw the count (0: none pending)
    long long changedAt;                   //   and when it last saw the count change
};

constinit ThreadQueue* gThreadQueues = nullptr; // under gQueuesLock
constinit thread_local ThreadQueue* tThreadQueue = nullptr;

void detachThread();
std::byte* prepareWrite(QuillLogger* logger, size_t size);

// Made right after the thread's Quill context, so it ends first
struct ThreadExit
{
    ~ThreadExit()
    {
        detachThread();
    }
};

Producer& producerOf(quill::detail::ThreadContext* context)
{
    return context->get_spsc_queue<QueueOptions::queue_type>().producer();
}

// The thread's last record ends where its next one starts (one as big as the ring wraps to 0); a copy has its size
size_t recordSize(const logdetail::Writer& writer, const ThreadQueue& thread, const unsigned char* record)
{
    if (record == thread.lastRecord.data())
        return thread.lastRecord.size();
    const size_t size = ((writer.position & writer.mask) - static_cast<size_t>(record - writer.storage)) & writer.mask;
    return size != 0 ? size : writer.mask + 1;
}

// Quill writes the queue only between handOver and takeOver, and may overwrite the last record, so it is copied first
void handOver(Producer& producer)
{
    logdetail::Writer& writer = *logdetail::tWriter;
    producer.writer_pos = writer.position;
    const unsigned char* last = writer.last.load(std::memory_order_relaxed);
    ThreadQueue& thread = *tThreadQueue;
    if (last != nullptr && last != thread.lastRecord.data())
        thread.lastRecord.assign(last, last + recordSize(writer, thread, last));
}

void takeOver(Producer& producer)
{
    logdetail::Writer& writer = *logdetail::tWriter;
    if (writer.last.load(std::memory_order_relaxed) != nullptr)
        writer.last.store(tThreadQueue->lastRecord.data(), std::memory_order_relaxed);
    writer.storage = reinterpret_cast<unsigned char*>(producer.storage);
    writer.mask = producer.mask;
    writer.position = producer.writer_pos;
    writer.limit.store(producer.reader_pos_cache_plus_capacity, std::memory_order_relaxed);
    writer.published = &producer.atomic_writer_pos;
}

// The record a thread's pending count repeats; null after the logger's end
const unsigned char* repeatedRecord(const ThreadQueue& thread)
{
    return thread.writer.last.load(std::memory_order_relaxed);
}

void writeEncodedRecord(unsigned char* at, const logdetail::Site& site, const std::vector<unsigned char>& args)
{
    const uint64_t time = logdetail::timestamp();
    const uint64_t tag = logdetail::recordTag(site);
    memcpy(at, &time, sizeof(time));
    memcpy(at + sizeof(time), &tag, sizeof(tag));
    memcpy(at + logdetail::kRecordHeader, args.data(), args.size());
}

// Under gQueuesLock, the calling thread's record of already encoded arguments: a full queue writes it synchronously
void writeEncoded(const logdetail::Site& site, const std::vector<unsigned char>& args)
{
    const size_t size = logdetail::kRecordHeader + args.size();
    logdetail::Writer& writer = *logdetail::tWriter;
    unsigned char* at = nullptr;
    if (logdetail::fits(writer, size))
        at = writer.storage + (writer.position & writer.mask);
    else if (!tQueueEnded && size <= gQueueMaxCapacity)
        at = reinterpret_cast<unsigned char*>(prepareWrite(gLogger.load(std::memory_order_relaxed), size));
    if (at == nullptr)
    {
        std::vector<unsigned char> record(size);
        writeEncodedRecord(record.data(), site, args);
        logdetail::writeSync(record.data());
        return;
    }
    writeEncodedRecord(at, site, args);
    writer.last.store(at, std::memory_order_relaxed);
    logdetail::publish(writer, size);
}

// The thread's count not yet written for it: a copy of the repeated record with the count where the timestamp is
void writePendingRepeat()
{
    logdetail::Writer& writer = *logdetail::tWriter;
    // ExitProcess ended the other threads, the logger thread too, maybe one holding a lock: nothing is written any more
    if (writer.repeats.load(std::memory_order_relaxed) == 0 || processExiting())
        return;
    ThreadQueue& thread = *tThreadQueue;
    // Held through the write: the logger's end, which takes the counts left, comes before this one is taken or after it
    QueuesLock lock;
    if (gLoggerEnded.load(std::memory_order_relaxed))
        return;
    const uint64_t count = writer.repeats.load(std::memory_order_relaxed) - thread.writtenRepeats;
    writer.repeats.store(0, std::memory_order_relaxed);
    thread.writtenRepeats = 0;
    thread.pendingSince = 0;
    if (count == 0)
        return;
    std::vector<unsigned char> args(logdetail::kRecordHeader);
    if (const unsigned char* repeated = repeatedRecord(thread))
        args.assign(repeated, repeated + recordSize(writer, thread, repeated));
    memcpy(args.data(), &count, sizeof(count));
    writeEncoded(kRepeatSite, args);
}

constexpr long long kRepeatCheckMs = 100;

// Under gQueuesLock: another thread's count as a line for writeLogLine; the thread counts on and later writes the rest
std::string repeatsLine(ThreadQueue& thread, uint64_t repeats, const unsigned char* repeated, long long now)
{
    static constexpr unsigned char kNotKnown[logdetail::kRecordHeader] {};
    std::string line(kLogPrefixSize, ' ');
    std::string text;
    formatRepeatCount(text, repeats - thread.writtenRepeats, repeated != nullptr ? repeated : kNotKnown);
    line += text;
    // Written over two checks later than its quiet delay, a count says how long before that its last repeat was
    if (thread.changedAt != 0 && ticksToMs(now - thread.changedAt) > repeatCountQuietMs() + 2 * kRepeatCheckMs)
        fmtStrAppend(line, " (the last %.3f s earlier)", ticksToTime(now - thread.changedAt));
    line += '\n';
    thread.writtenRepeats = repeats;
    thread.pendingSince = 0;
    return line;
}

void writeRepeatsFor(ThreadQueue& thread, uint64_t repeats, long long now)
{
    std::string line = repeatsLine(thread, repeats, repeatedRecord(thread), now);
    writeLogLine(line, ticksToTime(now), thread.threadId, thread.threadName, true);
}

// A thread's count taken at the logger's end, written after the lines queued before it
struct EndCount
{
    std::string line;
    uint32_t threadId;
    std::string threadName;
};

// Under gQueuesLock, for the logger's end: from here on only the calling thread logs, synchronously
std::vector<EndCount> closeQueues()
{
    std::vector<EndCount> counts;
    // Each thread's next LOG, a repeat too, does nothing; its count is taken now, before a LOG in flight adds
    const long long now = timeTicks();
    for (ThreadQueue* thread = gThreadQueues; thread != nullptr; thread = thread->next)
    {
        // The room first: a LOG finding no last record to repeat finds no room either
        thread->writer.limit.store(thread->writer.published->load(), std::memory_order_relaxed);
        // The count before the record: a thread holding a count replaces the record only under gQueuesLock
        const uint64_t repeats = thread->writer.repeats.load(std::memory_order_acquire);
        const unsigned char* last = thread->writer.last.exchange(nullptr, std::memory_order_release);
        if (repeats != thread->writtenRepeats)
            counts.push_back({repeatsLine(*thread, repeats, last, now), thread->threadId, thread->threadName});
    }
    // After the queues are closed: a line dropped here has no later line of its thread still queued after it
    stopSyncLog();
    gLoggerEnded.store(true, std::memory_order_release);
    return counts;
}

void writeCounts(std::vector<EndCount>& counts)
{
    for (EndCount& count : counts)
        writeLogLine(count.line, ticksToTime(timeTicks()), count.threadId, count.threadName, false);
}

// Under gQueuesLock as the logger ends, before a thread logging after the end gives up its queue and its place here
std::string threadsHoldingQueues()
{
    std::string threads;
    for (ThreadQueue* thread = gThreadQueues; thread != nullptr; thread = thread->next)
    {
        if (!threads.empty())
            threads += ", ";
        fmtStrAppend(threads, "%u", thread->threadId);
        if (thread->threadName[0] != '\0')
            fmtStrAppend(threads, " %s", thread->threadName);
    }
    return threads;
}

// The threads still holding a queue at the logger's end should have ended before it
void reportThreadsHoldingQueues(const std::string& threads)
{
    if (threads.empty())
        return;
    std::string line(kLogPrefixSize, ' ');
    line += "threads still holding a log queue at the logger's end: " + threads + '\n';
    char name[kThreadNameWidth + 1];
    readThreadName(name);
    writeLogLine(line, ticksToTime(timeTicks()), logThreadId(), name, false);
}

// The record's own time: where its counter is not bte's clock, now less the record's age at Quill's rate
double recordTime(const unsigned char* record)
{
    uint64_t recorded;
    memcpy(&recorded, record, sizeof(recorded));
    if constexpr (kRawTimestamps)
        return epochSeconds(recorded);
    else
    {
        // The rate first: Quill measures it at its first use, which takes 30 to 150 ms on an Intel Mac
        const double nsPerTick = quill::detail::RdtscClock::RdtscTicks::instance().ns_per_tick();
        const auto age = static_cast<int64_t>(logdetail::timestamp() - recorded);
        return ticksToTime(timeTicks()) - static_cast<double>(age) * nsPerTick / 1e9;
    }
}

// On the thread itself, before the logger thread reads: unread records with their own time and name
void writeUnread(quill::detail::ThreadContext* context)
{
    auto& queue = context->get_spsc_queue<QueueOptions::queue_type>();
    char threadName[kThreadNameWidth + 1];
    readThreadName(threadName);
    std::string name = threadName;
    std::string text;
    std::string line;
    for (auto read = queue.prepare_read(); read.read_pos != nullptr; read = queue.prepare_read())
    {
        const auto* record = reinterpret_cast<const unsigned char*>(read.read_pos);
        uint64_t tag;
        memcpy(&tag, record + sizeof(uint64_t), sizeof(tag));
        if (tag == reinterpret_cast<uintptr_t>(&kThreadNameMetadata))
        {
            uint32_t length;
            memcpy(&length, record + kQuillHeaderSize, sizeof(length));
            name.assign(reinterpret_cast<const char*>(record + kQuillHeaderSize + sizeof(length)), length);
            queue.finish_read(kQuillHeaderSize + sizeof(length) + length);
            continue;
        }
        if (tag == reinterpret_cast<uintptr_t>(&kThreadEndMetadata))
        {
            queue.finish_read(kQuillHeaderSize + sizeof(uint32_t));
            continue;
        }
        const unsigned char* end = formatQueued(tag, record + logdetail::kRecordHeader, text);
        queue.finish_read(static_cast<size_t>(end - record));
        line.assign(kLogPrefixSize, ' ');
        line += text;
        line += '\n';
        writeLogLine(line, recordTime(record), logThreadId(), name, false);
    }
    queue.commit_read();
}

// Under gQueuesLock: later LOGs write synchronously; until the logger thread reads, the queue's records go first
void endQueue()
{
    if (tQueueEnded)
        return;
    tQueueEnded = true;
    for (ThreadQueue** link = &gThreadQueues; *link != nullptr; link = &(*link)->next)
    {
        if (*link == tThreadQueue)
        {
            *link = tThreadQueue->next;
            break;
        }
    }
    if (tThreadQueue == nullptr)
        return;
    if (quill::detail::ThreadContext* context = quill::detail::FrontendThreadState::thread_context)
    {
        // Also before writeUnread: the context may be pooled for a new thread, which takes over Quill's producer state
        handOver(producerOf(context));
        if (!gLoggerEnded.load(std::memory_order_relaxed) && !gLoggerReading.load(std::memory_order_relaxed))
            writeUnread(context);
    }
    writePendingRepeat();
    logdetail::tWriter = &logdetail::kNoWriter;
    delete tThreadQueue;
    tThreadQueue = nullptr;
}

// Under gQueuesLock, so a full queue keeps the name: waiting for room would hold the lock the logger's first read takes
void sendThreadEnd()
{
    quill::detail::ThreadContext* context = quill::detail::FrontendThreadState::thread_context;
    QuillLogger* logger = gLogger.load(std::memory_order_relaxed);
    if (context == nullptr || logger == nullptr || tThreadQueue == nullptr ||
        gLoggerEnded.load(std::memory_order_relaxed))
    {
        return;
    }
    auto& queue = context->get_spsc_queue<QueueOptions::queue_type>();
    handOver(queue.producer());
    if (queue.prepare_write(kQuillHeaderSize + sizeof(uint32_t)) != nullptr)
        logger->log_statement<false>(&kThreadEndMetadata, std::string_view());
    takeOver(queue.producer());
}

void detachThread()
{
    // ExitProcess ended the other threads, maybe one holding gQueuesLock, and nothing reads the queues any more
    if (tQueueEnded || processExiting())
        return;
    writePendingRepeat();
    QueuesLock lock;
    sendThreadEnd();
    endQueue();
}

// Made by the thread's first call, with the queue's sizes as set then
quill::detail::ThreadContext* localThreadContext()
{
    return quill::detail::get_scoped_thread_context_impl(QueueOptions::queue_type, gQueueCapacity, gQueueMaxCapacity,
                                                         QueueOptions::huge_pages_policy);
}

// Under gQueuesLock, a thread's first LOG: its Quill context, then its name, which the OS holds, before its line
quill::detail::ThreadContext* attachThread(QuillLogger* logger)
{
    // A count of the thread's synchronous LLOGs goes before its first queued line
    writeSyncRepeats(true);
    quill::detail::ThreadContext* context = localThreadContext();
    quill::detail::FrontendThreadState::thread_context = context;
    thread_local ThreadExit threadExit;
    static_cast<void>(threadExit);
    ThreadQueue* thread = new ThreadQueue {{}, gThreadQueues, logThreadId(), {}, 0, {}, context, 0, 0, 0};
    gThreadQueues = thread;
    tThreadQueue = thread;
    logdetail::tWriter = &thread->writer;
    readThreadName(thread->threadName);
    logger->log_statement<false>(&kThreadNameMetadata, std::string_view(thread->threadName));
    takeOver(producerOf(context));
    return context;
}

// Before gQueuesLock, so threads making their first LOG together allocate their queues in parallel
void makeThreadContext()
{
    if (const QuillUse use; use)
        static_cast<void>(localThreadContext());
}

// setThreadName calls it; a thread that has not logged yet sends its name with its first LOG instead
void sendRenamedThreadName(const char* name)
{
    quill::detail::ThreadContext* context = quill::detail::FrontendThreadState::thread_context;
    if (context == nullptr || tQueueEnded)
        return;
    // The name record as writeUnread reads it
    const size_t size = kQuillHeaderSize + sizeof(uint32_t) + strlen(name);
    while (true)
    {
        {
            QueuesLock lock;
            QuillLogger* logger = gLogger.load(std::memory_order_relaxed);
            if (logger == nullptr || tQueueEnded)
                return;
            auto& queue = context->get_spsc_queue<QueueOptions::queue_type>();
            handOver(queue.producer());
            const bool room = queue.prepare_write(size) != nullptr;
            if (room)
            {
                snprintf(tThreadQueue->threadName, sizeof(tThreadQueue->threadName), "%s", name);
                logger->log_statement<false>(&kThreadNameMetadata, std::string_view(name));
            }
            takeOver(queue.producer());
            if (room)
                return;
        }
        // A full queue waits for the logger thread, whose first read waits for this lock
        std::this_thread::yield();
    }
}

void reportQuillError(const std::string& message)
{
    logSync(timeTicks(), message);
}

// At its first poll: Quill names no thread when thread names are off, and MSVC keeps a terminate handler per thread
void setUpLoggerThread()
{
    static constinit thread_local bool done = false;
    if (done)
        return;
    done = true;
    setThreadName("AsyncLog");
    setLoggerThreadCrashHooks();
}

// Repeats carry no timestamp, so the logger thread times a pending count with its own clock: it writes the count once
// the count stays the same for the quiet delay, or the maximum delay after it first saw it
void writeIdleRepeats()
{
    static long long lastCheck = 0;
    const long long now = timeTicks();
    // A crash flush wants every count now, as long as its line is written
    const uint64_t countsWanted = gCountsWanted.load(std::memory_order_acquire);
    const bool forced = countsWanted != gCountsWritten.load(std::memory_order_relaxed);
    if (!forced && ticksToMs(now - lastCheck) < kRepeatCheckMs)
        return;
    lastCheck = now;
    bool skipped = false;
    {
        const TriedQueuesLock lock;
        if (!lock.taken)
            return;
        // From the logger's end on, the end has taken the counts
        ThreadQueue* const first = gLoggerEnded.load(std::memory_order_relaxed) ? nullptr : gThreadQueues;
        for (ThreadQueue* thread = first; thread != nullptr; thread = thread->next)
        {
            // Acquired, as LOG releases it: the repeated record and its place in the queue are seen with the count
            const uint64_t repeats = thread->writer.repeats.load(std::memory_order_acquire);
            if (repeats == thread->writtenRepeats)
            {
                thread->pendingSince = 0;
                continue;
            }
            const bool changed = thread->pendingSince == 0 || repeats != thread->seenRepeats;
            if (thread->pendingSince == 0)
                thread->pendingSince = now;
            if (changed)
            {
                thread->seenRepeats = repeats;
                thread->changedAt = now;
            }
            // The repeated line itself goes out first: its record left the queue and its event the transit buffer
            quill::detail::ThreadContext* context = thread->context;
            const bool lineWritten =
                context->get_spsc_queue<QueueOptions::queue_type>().empty() && !context->has_transit_events();
            const bool quiet = ticksToMs(now - thread->changedAt) >= repeatCountQuietMs();
            const bool overdue = ticksToMs(now - thread->pendingSince) >= repeatCountMaxMs();
            if (lineWritten && (quiet || overdue || forced))
                writeRepeatsFor(*thread, repeats, now);
            else if (forced)
                skipped = true; // the request stays open until the line is written
        }
    }
    if (forced && !skipped)
        gCountsWritten.store(countsWanted, std::memory_order_release);
}

void startReading()
{
    if (gLoggerReading.load(std::memory_order_relaxed))
        return;
    static constinit bool firstPoll = true;
    if (firstPoll)
    {
        firstPoll = false;
        logTransition(LogTransition::AsyncFirstRead);
    }
    // No thread writes its own queue past the logger's end
    while (gQueuesLock.test_and_set(std::memory_order_acquire))
    {
        if (gLoggerEnded.load(std::memory_order_acquire))
            return;
        std::this_thread::yield();
    }
    gLoggerReading.store(true, std::memory_order_relaxed);
    gQueuesLock.clear(std::memory_order_release);
}

void onLoggerPoll()
{
    setUpLoggerThread();
    startReading();
    writeIdleRepeats();
}

// Under a QuillUse, so the backend runs until the request is served; one request, so later lines cannot hold it
void waitForQueuedLines()
{
    const uint64_t request = quill::Backend::request_flush_all();
    if (request == 0)
        return;
    while (!quill::Backend::wait_flush_all(request, std::chrono::seconds(1)))
        continue;
}

// False after a second without the first read, as when this thread's exit destructors hold Windows' loader lock
bool waitForFirstRead()
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!gLoggerReading.load(std::memory_order_acquire))
    {
        if (std::chrono::steady_clock::now() >= until)
            return false;
        std::this_thread::yield();
    }
    return true;
}

// The logger thread writes every pending count once its line is written, as its idle check does; a second at most
void waitForPendingCounts()
{
    if (quill::Backend::get_thread_id() == logThreadId())
        return;
    const uint64_t wanted = gCountsWanted.fetch_add(1, std::memory_order_acq_rel) + 1;
    quill::Backend::notify();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (gCountsWritten.load(std::memory_order_acquire) < wanted && std::chrono::steady_clock::now() < until)
        std::this_thread::yield();
}

// One request: the lines queued now, not those other threads keep logging meanwhile; crashWaitMs without a line ends it
void waitWhileLinesWritten()
{
    const uint64_t request = quill::Backend::request_flush_all();
    uint64_t written = gLinesWritten.load(std::memory_order_relaxed);
    while (!quill::Backend::wait_flush_all(request, std::chrono::milliseconds(crashWaitMs())))
    {
        const uint64_t now = gLinesWritten.load(std::memory_order_relaxed);
        if (now == written)
            break;
        written = now;
    }
}

// Until the logger thread reads, the thread writes its own queued lines; false once it reads
bool writeOwnQueuedLines()
{
    QueuesLock lock;
    // Past the logger's end the logger thread may read the queues unlocked, and this thread's line is dropped
    if (gLoggerEnded.load(std::memory_order_relaxed))
        return true;
    if (gLoggerReading.load(std::memory_order_relaxed))
        return false;
    quill::detail::ThreadContext* context = quill::detail::FrontendThreadState::thread_context;
    // A context and no ended queue mean the thread's queue is listed
    if (context != nullptr && !tQueueEnded)
    {
        handOver(producerOf(context));
        writeUnread(context);
        // writeUnread took the name record too, and the logger thread names the thread's later lines only from one
        gLogger.load(std::memory_order_relaxed)->log_statement<false>(&kThreadNameMetadata,
                                                                       std::string_view(tThreadQueue->threadName));
        takeOver(producerOf(context));
    }
    return true;
}

// The thread's queued lines go first, however long it takes; after endQueue there are none
void writeQueuedLinesFirst()
{
    if (!gLoggerReading.load(std::memory_order_acquire) && writeOwnQueuedLines())
        return;
    waitForQueuedLines();
}

// LLOG's record, built on its caller's stack, goes into the thread's queue as LOG's record would
void queueLlogRecord(const unsigned char* record, size_t size)
{
    logdetail::Writer& current = *logdetail::tWriter;
    const unsigned char* last = current.last.load(std::memory_order_relaxed);
    const size_t tagged = size - sizeof(uint64_t);
    // A last record means the thread has its queue: kNoWriter has none, and endQueue resets both together
    if (last != nullptr && recordSize(current, *tThreadQueue, last) == size &&
        memcmp(last + sizeof(uint64_t), record + sizeof(uint64_t), tagged) == 0)
    {
        current.repeats.store(current.repeats.load(std::memory_order_relaxed) + 1, std::memory_order_release);
        return;
    }
    logdetail::acquireIfCleared(last);
    unsigned char* at = logdetail::fits(current, size) ? current.storage + (current.position & current.mask)
                                                        : logdetail::reserve(size);
    if (at == nullptr)
    {
        logdetail::writeSync(record);
        return;
    }
    // reserve gives a thread's first record its queue, and with it its own Writer
    logdetail::Writer& writer = *logdetail::tWriter;
    memcpy(at, record, size);
    if (writer.repeats.load(std::memory_order_relaxed) != 0)
    {
        logdetail::commitAfterRepeats(at, size);
        return;
    }
    writer.last.store(at, std::memory_order_relaxed);
    logdetail::publish(writer, size);
}

void decodeRecord(std::byte*& data, quill::DynamicFormatArgStore& store);

// Registered at DestructionManager level 10 before main, started by the first LOG
class AsyncLog
{
public:
    AsyncLog() = default;

    void start()
    {
#ifdef __MINGW32__
        // MinGW's first float format calls atexit, which on the logger thread waits for a running exit()'s lock
        static_cast<void>(fmtStr("%f", 0.0));
#endif
        globalState = std::make_unique<quill::GlobalState>(); // must exist before Quill starts
        std::shared_ptr<quill::Sink> sink = Frontend::create_or_get_sink<LogSink>("log");
        QuillLogger* logger = Frontend::create_or_get_logger("log", sink,
                                                             quill::PatternFormatterOptions {"%(message)"});
        quill::BackendOptions options;
        options.check_printable_char = {}; // a line keeps its tabs and newlines
        options.raw_rdtsc_timestamps = kRawTimestamps;
        options.error_notifier = reportQuillError;
        options.backend_worker_on_poll_begin = onLoggerPoll;
        options.compact_decoder = decodeRecord;
        options.compact_metadata = &kLineMetadata;
        options.compact_logger = logger;
        quill::Backend::start(options);
        logTransition(LogTransition::AsyncRunning);
        // Before the redirect, so an LLOG on this thread finds the logger running instead of starting it again
        gLogger.store(logger, std::memory_order_release);
        logTransition(LogTransition::AsyncPublished);
        setLlogRedirect(queueLlogRecord, flushAsyncLog);
        setCrashHooks();
        setThreadNameListener(sendRenamedThreadName);
    }

    AsyncLog(const AsyncLog&) = delete;
    AsyncLog& operator=(const AsyncLog&) = delete;

    ~AsyncLog()
    {
        logTransition(LogTransition::AsyncEnd);
        detachThread();
        bool started = false;
        {
            QueuesLock lock; // start() assigns globalState under it
            started = globalState != nullptr;
        }
        // Twice: the second writes what threads still running logged while the first was writing
        if (started)
        {
            waitForQueuedLines();
            waitForQueuedLines();
        }
        logTransition(LogTransition::AsyncEndWaited);
        std::vector<EndCount> counts;
        std::string holding;
        {
            QueuesLock lock;
            counts = closeQueues();
            holding = threadsHoldingQueues();
            // Null after a failed start, which installed none of these
            if (gLogger.exchange(nullptr) != nullptr)
            {
                setThreadNameListener(nullptr);
                setLlogRedirect(nullptr, nullptr);
                unsetCrashHooks();
            }
        }
        // Unlocked: a Windows thread ending meanwhile holds the loader lock, which the logger thread needs to end
        if (globalState != nullptr)
        {
            while (gQuillUsers.load() != 0)
                std::this_thread::yield();
            globalState.reset(); // stops the logger thread once it wrote every queued line
        }
        logTransition(LogTransition::AsyncEnded);
        writeCounts(counts);
        reportThreadsHoldingQueues(holding);
        flushSyncLog();
    }

    void storeRecord(std::byte*& data, quill::DynamicFormatArgStore& store)
    {
        uint64_t tag;
        memcpy(&tag, data, sizeof(tag));
        const auto* args = reinterpret_cast<const unsigned char*>(data + sizeof(tag));
        data += sizeof(tag) + static_cast<size_t>(formatQueued(tag, args, loggerThreadLine) - args);
        store.push_back(fmtquill::string_view {loggerThreadLine.data(), loggerThreadLine.size()});
    }

private:
    std::unique_ptr<quill::GlobalState> globalState;
    std::string loggerThreadLine; // the logger thread's buffer; the destructor stops that thread before members go
};

AsyncLog* registeredAsyncLog()
{
    static AsyncLog* const asyncLog = DestructionManager::make_shared<AsyncLog>(10).get();
    return asyncLog;
}

// Quill's logger thread calls it for each record LOG wrote
void decodeRecord(std::byte*& data, quill::DynamicFormatArgStore& store)
{
    registeredAsyncLog()->storeRecord(data, store);
}

// Even if nothing logs until teardown, a LOG from a dtor below level 10 must find the logger ended
[[maybe_unused]] const bool gAsyncLogRegistered = (registeredAsyncLog(), true);

// Locked: the logger's end waits for a start in progress, and a start that comes after the end does nothing
bool startUnlessEnded()
{
    QueuesLock lock;
    if (gLoggerEnded.load(std::memory_order_relaxed))
        return true;
    try
    {
        logTransition(LogTransition::AsyncStart);
        registeredAsyncLog()->start();
    }
    catch (const std::exception& error)
    {
        // Ended, never retried: a second start would replace the GlobalState, whose destructor frees Quill's managers
        gLoggerEnded.store(true, std::memory_order_release);
        logSync(timeTicks(), std::string("the async logger did not start: ") + error.what());
    }
    return true;
}

NOINLINE QuillLogger* startAsyncLog()
{
    if (gLoggerEnded.load(std::memory_order_acquire))
        return nullptr;
    static const bool started = startUnlessEnded();
    static_cast<void>(started);
    return gLogger.load(std::memory_order_acquire);
}

// Locked, so the logger cannot end between the check and the write
std::byte* prepareWrite(QuillLogger* logger, size_t size)
{
    QueuesLock lock;
    if (gLoggerEnded.load(std::memory_order_relaxed))
    {
        endQueue();
        return nullptr;
    }
    quill::detail::ThreadContext* context = quill::detail::FrontendThreadState::thread_context;
    if (context == nullptr)
        context = attachThread(logger);
    auto& queue = context->get_spsc_queue<QueueOptions::queue_type>();
    handOver(queue.producer());
    std::byte* at = queue.prepare_write(size);
    takeOver(queue.producer());
    return at;
}

} // namespace

void setLogQueueCapacity(size_t initial, size_t max)
{
    gQueueCapacity = initial;
    gQueueMaxCapacity = max;
}

void flushAsyncLog()
{
    if (syncLogStopped() || processExiting())
        return;
    writeSyncRepeats(false);
    writePendingRepeat();
    if (const QuillUse use; use)
    {
        if (waitForFirstRead())
        {
            waitForQueuedLines();
            waitForPendingCounts();
        }
        else
            writeQueuedLinesFirst();
    }
    flushSyncLog();
}

void flushAsyncLogForCrash()
{
    writeSyncRepeats(false);
    if (const QuillUse use; use)
    {
        waitWhileLinesWritten();
        waitForPendingCounts();
        // A thread logging a different line meanwhile moved its count into its queue, past the first wait
        waitWhileLinesWritten();
    }
    flushSyncLog();
}

void writeLineBeingFormatted(const char* what)
{
    const unsigned char* args = tFormattingArgs;
    if (args == nullptr)
        return;
    // Once, though several handlers see the same crash
    tFormattingArgs = nullptr;
    std::string text;
    failedLine(tFormattingTag, args, what, text);
    std::string line(kLogPrefixSize, ' ');
    line += text;
    line += '\n';
    char name[kThreadNameWidth + 1];
    readThreadName(name);
    writeLogLine(line, ticksToTime(timeTicks()), logThreadId(), name, false);
    flushSyncLog();
}

unsigned char* logdetail::reserve(size_t size)
{
    if (tQueueEnded || gLoggerEnded.load() || size > gQueueMaxCapacity || processExiting() || exitingWithoutQueue())
        return nullptr;
    QuillLogger* logger = gLogger.load(std::memory_order_acquire);
    if (logger == nullptr)
        logger = startAsyncLog();
    if (quill::detail::FrontendThreadState::released)
        return nullptr;
    if (quill::detail::FrontendThreadState::thread_context == nullptr)
        makeThreadContext();
    std::byte* at = prepareWrite(logger, size);
    while (at == nullptr && !tQueueEnded)
    {
        std::this_thread::yield();
        at = prepareWrite(logger, size);
    }
    return reinterpret_cast<unsigned char*>(at);
}

// The record differs from the line its thread repeated: the count goes first
void logdetail::commitAfterRepeats(unsigned char* record, size_t size)
{
    const std::vector<unsigned char> copy(record, record + size);
    writePendingRepeat();
    Writer& writer = *tWriter;
    unsigned char* at = fits(writer, size) ? writer.storage + (writer.position & writer.mask) : reserve(size);
    if (at == nullptr)
    {
        writeSync(copy.data());
        return;
    }
    memcpy(at, copy.data(), size);
    writer.last.store(at, std::memory_order_relaxed);
    publish(writer, size);
}

void logdetail::writeSync(const unsigned char* record)
{
    if (syncLinesDropped())
        return;
    const long long logTime = timeTicks(); // not the end of the wait below
    writeSyncRepeats(true);
    writePendingRepeat();
    // The thread's next record comes after this line, so it is no repeat of a record queued before it
    if (tWriter->last.load(std::memory_order_relaxed) != nullptr)
        tWriter->last.store(nullptr, std::memory_order_relaxed);
    if (const QuillUse use; use && quill::Backend::get_thread_id() != logThreadId())
        writeQueuedLinesFirst();
    uint64_t tag;
    memcpy(&tag, record + sizeof(uint64_t), sizeof(tag));
    std::string text;
    siteOf(tag).format(record + kRecordHeader, text);
    logSync(logTime, text);
}

void logdetail::submitText(std::string_view text)
{
    logFields(kTextSite, text);
}

#ifdef _WIN32
// In the TLS directory's callbacks, which nothing references, so the linker is told to keep it
#ifdef _MSC_VER
#ifdef _M_IX86
#pragma comment(linker, "/INCLUDE:__tls_used")
#pragma comment(linker, "/INCLUDE:_gLogThreadExitCallback")
#else
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:gLogThreadExitCallback")
#endif
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK gLogThreadExitCallback = markThreadExiting;
#pragma const_seg()
#else
extern "C" const PIMAGE_TLS_CALLBACK gLogThreadExitCallback __attribute__((section(".CRT$XLB"), used)) =
    markThreadExiting;
#endif
#endif
