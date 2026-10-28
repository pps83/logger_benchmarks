// log_stress: children make the logger's lifetime transitions while threads log through them; it checks their logs
#include "AsyncLog.h"
#include "DestructionManager.h"
#include "SyncLog.h"
#include "debug.h"
#include "random.h"
#include "sysutil.h"
#include "TestFile.h"
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _MSC_VER
#include <crtdbg.h>
#endif

namespace fs = std::filesystem;

// A custom type, so LOG formats its line at the call
struct LineNumber
{
    int value;
};

// A worker released after the logger's end, whose lines the logger ignores without formatting them
static thread_local bool tIgnored = false;

template <>
struct std::formatter<LineNumber> : std::formatter<int>
{
    auto format(const LineNumber& number, std::format_context& context) const
    {
        // Anything on stderr fails the run
        if (tIgnored)
            fputs("an ignored LOG formatted its line\n", stderr);
        return std::formatter<int>::format(number.value, context);
    }
};

namespace
{

// What the child's main thread does around the transition, and with it what the log must hold
enum class Phase { Open, Start, End, Teardown, Crash };

// Where the scenario acts: at one of the logger's transitions, or at a point of the exit the child marks itself
enum class Point
{
    SyncLogOpen,
    AsyncStart,
    AsyncRunning,
    AsyncPublished,
    AsyncFirstRead,
    Main,             // in main, while the workers log
    ExitStart,        // a static object's destructor, before DestructionManager's objects
    DestructionAbove, // a DestructionManager object above the logger's level
    AsyncEnd,
    AsyncEndWaited,
    AsyncEnded,
    DestructionBelow, // one below it, after the logger's end
    SyncLogClose,
    SyncLogClosed,
    AfterClose,       // one below SyncLog's level, after the log file closed
};

// Drawn, for each run: a fault, an abort, or a throw on a worker and a std::terminate on the thread at the point
enum class Crash { None, Fault, Abort, Throw, Terminate, Drawn };

struct Scenario
{
    const char* name;
    Phase phase;
    Point releaseAt; // Teardown and Crash release the workers themselves
    Crash crash = Crash::None; // at the point, on the thread making it or on a worker
    bool onWorker = false;
    bool abandon = false; // the workers are never stopped: they log on through the exit, every kind of statement
};

constexpr Scenario kScenarios[] = {
    {"SyncLogOpen", Phase::Open, Point::SyncLogOpen},
    {"AsyncStart", Phase::Start, Point::AsyncStart},
    {"AsyncRunning", Phase::Start, Point::AsyncRunning},
    {"AsyncPublished", Phase::Start, Point::AsyncPublished},
    {"AsyncFirstRead", Phase::Start, Point::AsyncFirstRead},
    {"Teardown", Phase::Teardown, Point::AsyncEnd},
    {"ExitStart", Phase::End, Point::ExitStart},
    {"DestructionAbove", Phase::End, Point::DestructionAbove},
    {"AsyncEnd", Phase::End, Point::AsyncEnd},
    {"AsyncEndWaited", Phase::End, Point::AsyncEndWaited},
    {"AsyncEnded", Phase::End, Point::AsyncEnded},
    {"DestructionBelow", Phase::End, Point::DestructionBelow},
    {"Abandoned", Phase::End, Point::AsyncEnd, Crash::None, false, true},
    {"CrashMainFault", Phase::Crash, Point::Main, Crash::Fault},
    {"CrashMainAbort", Phase::Crash, Point::Main, Crash::Abort},
    {"CrashMainThrow", Phase::Crash, Point::Main, Crash::Throw},
    {"CrashWorkerFault", Phase::Crash, Point::Main, Crash::Fault, true},
    {"CrashWorkerAbort", Phase::Crash, Point::Main, Crash::Abort, true},
    {"CrashWorkerThrow", Phase::Crash, Point::Main, Crash::Throw, true},
    {"CrashAtSyncLogOpen", Phase::Open, Point::SyncLogOpen, Crash::Drawn},
    {"CrashAtAsyncStart", Phase::Start, Point::AsyncStart, Crash::Drawn},
    {"CrashAtAsyncRunning", Phase::Start, Point::AsyncRunning, Crash::Drawn},
    {"CrashAtAsyncPublished", Phase::Start, Point::AsyncPublished, Crash::Drawn},
    {"CrashAtAsyncFirstRead", Phase::Start, Point::AsyncFirstRead, Crash::Drawn},
    {"CrashAtExitStart", Phase::End, Point::ExitStart, Crash::Drawn},
    {"CrashAtDestructionAbove", Phase::End, Point::DestructionAbove, Crash::Drawn},
    {"CrashAtAsyncEnd", Phase::End, Point::AsyncEnd, Crash::Drawn},
    {"CrashAtAsyncEndWaited", Phase::End, Point::AsyncEndWaited, Crash::Drawn},
    {"CrashAtAsyncEnded", Phase::End, Point::AsyncEnded, Crash::Drawn},
    {"CrashAtDestructionBelow", Phase::End, Point::DestructionBelow, Crash::Drawn},
    {"CrashAtSyncLogClose", Phase::End, Point::SyncLogClose, Crash::Drawn},
    {"CrashAtSyncLogClosed", Phase::End, Point::SyncLogClosed, Crash::Drawn},
    {"CrashAtAfterClose", Phase::End, Point::AfterClose, Crash::Drawn},
    {"WorkerCrashAtSyncLogOpen", Phase::Open, Point::SyncLogOpen, Crash::Drawn, true},
    {"WorkerCrashAtAsyncStart", Phase::Start, Point::AsyncStart, Crash::Drawn, true},
    {"WorkerCrashAtAsyncRunning", Phase::Start, Point::AsyncRunning, Crash::Drawn, true},
    {"WorkerCrashAtAsyncPublished", Phase::Start, Point::AsyncPublished, Crash::Drawn, true},
    {"WorkerCrashAtAsyncFirstRead", Phase::Start, Point::AsyncFirstRead, Crash::Drawn, true},
    {"WorkerCrashAtExitStart", Phase::End, Point::ExitStart, Crash::Drawn, true},
    {"WorkerCrashAtDestructionAbove", Phase::End, Point::DestructionAbove, Crash::Drawn, true},
    {"WorkerCrashAtAsyncEnd", Phase::End, Point::AsyncEnd, Crash::Drawn, true},
    {"WorkerCrashAtAsyncEndWaited", Phase::End, Point::AsyncEndWaited, Crash::Drawn, true},
    {"WorkerCrashAtAsyncEnded", Phase::End, Point::AsyncEnded, Crash::Drawn, true},
    {"WorkerCrashAtDestructionBelow", Phase::End, Point::DestructionBelow, Crash::Drawn, true},
};

bool crashes(const Scenario& scenario)
{
    return scenario.crash != Crash::None;
}

// From the async logger's end on, other threads' LOGs are ignored: a worker released there logs nothing more
bool pastTheEnd(Point point)
{
    return point >= Point::AsyncEnded;
}

// Whether the log's crash handlers are surely installed there: from the end of the async logger's start to its end
bool handlersAt(Point point)
{
    switch (point)
    {
    case Point::Main:
    case Point::ExitStart:
    case Point::DestructionAbove:
    case Point::AsyncEnd:
    case Point::AsyncEndWaited:
        return true;
    case Point::SyncLogOpen:
    case Point::AsyncStart:
    case Point::AsyncRunning:
    case Point::AsyncPublished:
    case Point::AsyncFirstRead:
    case Point::AsyncEnded:
    case Point::DestructionBelow:
    case Point::SyncLogClose:
    case Point::SyncLogClosed:
    case Point::AfterClose:
        return false;
    }
    return false;
}

// The run's crash: a Drawn one comes from the seed, the same in the child and in the parent checking its log
Crash crashOf(const Scenario& scenario, uint64_t seed)
{
    if (scenario.crash != Crash::Drawn)
        return scenario.crash;
    // Without the log's handlers an abort is the C runtime's alone: MSVC Debug's runs the exit as other threads go on
    if (!handlersAt(scenario.releaseAt))
        return Crash::Fault;
    rand_default_t random;
    srand64_r(random, seed ^ 0x5bd1e9955bd1e995ull);
    const Crash kinds[] = {Crash::Fault, Crash::Abort, scenario.onWorker ? Crash::Throw : Crash::Terminate};
    return kinds[rand64_r(random) % std::size(kinds)];
}

// The logger runs that point under its own locks, or on its own thread: the crash there logs nothing first
bool pointLogs(Point point)
{
    switch (point)
    {
    case Point::SyncLogOpen:
    case Point::AsyncStart:
    case Point::AsyncRunning:
    case Point::AsyncPublished:
    case Point::AsyncFirstRead:
        return false;
    case Point::Main:
    case Point::ExitStart:
    case Point::DestructionAbove:
    case Point::AsyncEnd:
    case Point::AsyncEndWaited:
    case Point::AsyncEnded:
    case Point::DestructionBelow:
    case Point::SyncLogClose:
    case Point::SyncLogClosed:
    case Point::AfterClose:
        return true;
    }
    return true;
}

// The points after JoinsWorkers' level, which stops the workers first
bool afterTheWorkers(Point point)
{
    return point >= Point::SyncLogClose;
}

// Whether a crash there keeps every earlier line: the crash hooks run with the async logger, the close puts all on disk
bool crashKeepsLines(const Scenario& scenario)
{
    switch (scenario.releaseAt)
    {
    case Point::Main:
    case Point::ExitStart:
    case Point::DestructionAbove:
    case Point::AsyncEnd:
    case Point::AsyncEndWaited:
    case Point::SyncLogClosed:
    case Point::AfterClose:
        return true;
    case Point::SyncLogOpen:
    case Point::AsyncStart:
    case Point::AsyncRunning:
    case Point::AsyncPublished:
    case Point::AsyncFirstRead:
    case Point::AsyncEnded:
    case Point::DestructionBelow:
    case Point::SyncLogClose:
        return false;
    }
    return false;
}

// The "stress crash" lines a crash keeping its lines leaves: none where it logs nothing first, nor after the close
int keptCrashLines(const Scenario& scenario)
{
    if (!pointLogs(scenario.releaseAt))
        return 0;
    if (scenario.releaseAt == Point::SyncLogClosed || scenario.releaseAt == Point::AfterClose)
        return 0;
    return 1;
}

const Scenario* findScenario(std::string_view name)
{
    for (const Scenario& scenario : kScenarios)
    {
        if (name == scenario.name)
            return &scenario;
    }
    return nullptr;
}

constexpr int kWorkers = 16;

// The iterations a worker logs (drawIteration says what each one logs) and how its lines may end
struct WorkerPlan
{
    int before;      // iterations before the release
    int after;       // and after it
    bool llogBefore; // LLOG only, which never starts the async logger
    bool llogAfter;
    bool cut;        // the logger's end or a crash drops the lines that come after it
    bool stays;      // until the workers are stopped, instead of ending after its iterations
};

WorkerPlan workerPlan(Phase phase, int worker)
{
    switch (phase)
    {
    case Phase::Open:
        return {0, 200, true, true, false, false};
    case Phase::Start:
        return {20, 200, true, false, false, false};
    case Phase::End:
    case Phase::Teardown:
        // Its first LOG at the transition or a queue and a pending count then, and it ends during the transition or not
        return {worker % 2 == 0 ? 20 : 0, 200, false, false, true, worker % 4 < 2};
    case Phase::Crash:
        return {worker % 2 == 0 ? 20 : 0, 200, false, false, true, true};
    }
    return {};
}

constexpr int kCrashingWorker = 2;

// The main thread's "stress main again" lines, those lines with their repeats, and its "stress joined" lines
struct MainLines
{
    int againLinesMin;
    int againLinesMax;
    int againTotal;
    int joined;
};

MainLines mainLines(const Scenario& scenario)
{
    switch (scenario.phase)
    {
    case Phase::Open:
        return {0, 0, 0, 0};
    case Phase::Start:
        return {2, 2, 4, 0};
    case Phase::End:
        // Abandoned workers have no JoinsWorkers to log its pair and "joined"
        if (scenario.abandon)
            return {2, 2, 4, 0};
        // Where the runtime ran the main thread's thread_local destructors before the teardown, LLOG counts no repeats
        return {3, 4, 6, 1};
    case Phase::Teardown:
        return {3, 3, 6, 1};
    case Phase::Crash:
        return {2, 2, 4, 0};
    }
    return {};
}

// The lines PointMarker logs at the exit's start, above the logger, below it and after the close
struct MarkedLines
{
    int exitStart;
    int above;
    int below;
    int afterClose;
};

// The exiting thread logs until the close, synchronously after the logger's end, and nothing after it
MarkedLines markedLines(const Scenario& scenario)
{
    return scenario.phase == Phase::End ? MarkedLines {1, 1, 1, 0} : MarkedLines {0, 0, 0, 0};
}

const Scenario* gScenario = nullptr;
uint64_t gSeed = 0;
std::chrono::microseconds gPause {0};
int gCrashAt = 0; // the crashing worker's iteration after the release
std::atomic<bool> gReleased {false};
std::atomic<bool> gStopped {false};
std::atomic<int> gWaiting {0};

// How a worker logs its numbered line; Long adds a padding, at times over the queue's limit
enum class Kind { Printf, Format, String, CString, Custom, Long };

constexpr int kHelperLines = 5;

// What one iteration of a worker logs, drawn the same way by the worker and by the checker
struct Iteration
{
    Kind kind;
    int padding;
    bool llog;
    int repeats; // the same statement again, counted instead of written: 0, or 1 to 9 for 5% of the lines
    bool literal; // a lone literal after the numbered line
    bool literalLlog;
    int literalRepeats;
    bool helper; // a thread that logs kHelperLines lines and ends, joined before the next iteration
    bool rename;
    bool flush; // LOG(), where LOG is allowed
};

uint64_t workerSeed(uint64_t seed, int worker)
{
    return (seed ^ (0x9e3779b97f4a7c15ull * static_cast<uint64_t>(worker + 1))) | 1;
}

int drawRepeats(rand_default_t& random)
{
    const uint64_t draw = rand64_r(random);
    return draw % 100 < 5 ? 1 + static_cast<int>(draw / 100 % 9) : 0;
}

Iteration drawIteration(rand_default_t& random, bool llogOnly)
{
    Iteration iteration {};
    const uint64_t kind = rand64_r(random);
    iteration.kind = kind % 100 == 0 ? Kind::Long : static_cast<Kind>(kind / 100 % 5);
    // Over 512 bytes LLOG builds its record on the heap; over 16 KiB, log_stress's queue limit, it is written at once
    iteration.padding = kind / 500 % 2 == 0 ? 600 : 20000;
    const uint64_t flags = rand64_r(random);
    iteration.llog = llogOnly || flags % 2 == 0;
    iteration.literal = flags / 2 % 10 == 0;
    iteration.literalLlog = llogOnly || flags / 20 % 2 == 0;
    iteration.helper = flags / 40 % 100 == 0;
    iteration.rename = flags / 4000 % 100 == 0;
    iteration.flush = !llogOnly && flags / 400000 % 50 == 0;
    iteration.repeats = drawRepeats(random);
    iteration.literalRepeats = drawRepeats(random);
    return iteration;
}

std::string numberedLine(int worker, int number, const Iteration& iteration)
{
    std::string line = fmtStr("stress %d line %d", worker, number);
    if (iteration.kind == Kind::Long)
        line += " " + std::string(static_cast<size_t>(iteration.padding), 'p');
    return line;
}

// The same statement each time, so that after the first only its count is written
void logNumbered(int worker, int number, const Iteration& iteration, int times)
{
    const std::string text = std::to_string(number);
    const std::string padding(iteration.kind == Kind::Long ? static_cast<size_t>(iteration.padding) : 0, 'p');
    for (int n = 0; n < times; ++n)
    {
        switch (iteration.kind)
        {
        case Kind::Printf:
            if (iteration.llog)
                LLOG("stress %d line %d", worker, number);
            else
                LOG("stress %d line %d", worker, number);
            break;
        case Kind::Format:
            if (iteration.llog)
                LLOG("stress {} line {}", worker, number);
            else
                LOG("stress {} line {}", worker, number);
            break;
        case Kind::String:
            if (iteration.llog)
                LLOG("stress {} line {}", worker, text);
            else
                LOG("stress {} line {}", worker, text);
            break;
        case Kind::CString:
            if (iteration.llog)
                LLOG("stress %d line %s", worker, text.c_str());
            else
                LOG("stress %d line %s", worker, text.c_str());
            break;
        case Kind::Custom:
            if (iteration.llog)
                LLOG("stress {} line {}", worker, LineNumber {number});
            else
                LOG("stress {} line {}", worker, LineNumber {number});
            break;
        case Kind::Long:
            if (iteration.llog)
                LLOG("stress {} line {} {}", worker, number, padding);
            else
                LOG("stress {} line {} {}", worker, number, padding);
            break;
        }
    }
}

void logLiteral(bool llog, int times)
{
    for (int n = 0; n < times; ++n)
    {
        if (llog)
            LLOG("stress literal");
        else
            LOG("stress literal");
    }
}

// Ends while the others log, so its queue goes back to the logger for a new thread
void logHelper(int worker, int helper, bool llog)
{
    for (int line = 0; line < kHelperLines; ++line)
    {
        if (llog)
            LLOG("stress %d helper %d line %d", worker, helper, line);
        else
            LOG("stress %d helper %d line %d", worker, helper, line);
    }
}

std::string workerName(int worker)
{
    return fmtStr("stress %d", worker);
}

std::string renamedWorker(int worker, int number)
{
    return fmtStr("stress %d r%d", worker, number);
}

void logIteration(int worker, int number, const Iteration& iteration, int& helpers)
{
    logNumbered(worker, number, iteration, 1 + iteration.repeats);
    if (iteration.literal)
        logLiteral(iteration.literalLlog, 1 + iteration.literalRepeats);
    if (iteration.helper)
    {
        // Once a crash scenario releases its workers, a thread started as the crash ends the process fails under MinGW
        if (crashes(*gScenario) && gReleased.load())
            ++helpers;
        else
            std::thread(logHelper, worker, helpers++, iteration.llog).join();
    }
    if (iteration.rename)
        setThreadName(renamedWorker(worker, number).c_str());
    if (iteration.flush)
        LOG();
}

// Somewhere in the first page, never mapped, picked at run time: icx removed a write through a null it could see
volatile int* volatile gNowhere = nullptr;

// Where the crash logs its line first: LLOG in the scenarios before the async logger, none under the logger's locks
enum class CrashLine { None, Log, Llog };

Crash gCrash = Crash::None; // the run's own, crashOf's draw for Drawn

void crash(Crash how, CrashLine line)
{
    // The log must hold it where it can, so a failure before the planned crash does not pass for it
    if (line == CrashLine::Log)
        LOG("stress crash");
    else if (line == CrashLine::Llog)
        LLOG("stress crash");
    switch (how)
    {
    case Crash::Fault:
        *gNowhere = 1;
        break;
    case Crash::Abort:
        abort();
    case Crash::Throw:
        throw std::runtime_error("log_stress crash");
    case Crash::Terminate:
        std::terminate();
    case Crash::None:
    case Crash::Drawn:
        break;
    }
}

char gTailPadding[20000]; // no destructor: abandoned workers read it until the process ends

// An abandoned worker logs every kind of statement until the process ends it, allocating nothing itself
[[noreturn]] void logTail(int worker, rand_default_t& random)
{
    const std::string_view text = "text";
    for (int number = 0;; ++number)
    {
        const Iteration iteration = drawIteration(random, false);
        const std::string_view padding(gTailPadding, static_cast<size_t>(iteration.padding));
        switch (iteration.kind)
        {
        case Kind::Printf:
            if (iteration.llog)
                LLOG("stress %d tail %d", worker, number);
            else
                LOG("stress %d tail %d", worker, number);
            break;
        case Kind::Format:
            if (iteration.llog)
                LLOG("stress {} tail {}", worker, number);
            else
                LOG("stress {} tail {}", worker, number);
            break;
        case Kind::String:
            if (iteration.llog)
                LLOG("stress {} tail {}", worker, text);
            else
                LOG("stress {} tail {}", worker, text);
            break;
        case Kind::CString:
            if (iteration.llog)
                LLOG("stress %d tail %s", worker, text.data());
            else
                LOG("stress %d tail %s", worker, text.data());
            break;
        case Kind::Custom:
            if (iteration.llog)
                LLOG("stress {} tail {}", worker, LineNumber {number});
            else
                LOG("stress {} tail {}", worker, LineNumber {number});
            break;
        case Kind::Long:
            if (iteration.llog)
                LLOG("stress {} tail {} {}", worker, number, padding);
            else
                LOG("stress {} tail {} {}", worker, number, padding);
            break;
        }
    }
}

void work(int worker)
{
    const WorkerPlan plan = workerPlan(gScenario->phase, worker);
    setThreadName(workerName(worker).c_str());
    rand_default_t random;
    srand64_r(random, workerSeed(gSeed, worker));
    int number = 0;
    int helpers = 0;
    for (; number < plan.before; ++number)
        logIteration(worker, number, drawIteration(random, plan.llogBefore), helpers);
    gWaiting.fetch_add(1);
    while (!gReleased.load())
        std::this_thread::yield();
    tIgnored = gScenario->phase == Phase::End && pastTheEnd(gScenario->releaseAt);
    if (gScenario->abandon)
        logTail(worker, random);
    for (int done = 0; done < plan.after && !gStopped.load(); ++done)
    {
        if (gScenario->onWorker && worker == kCrashingWorker && done == gCrashAt)
            crash(gCrash, plan.llogAfter ? CrashLine::Llog : CrashLine::Log);
        logIteration(worker, number++, drawIteration(random, plan.llogAfter), helpers);
    }
    while (plan.stays && !gStopped.load())
        std::this_thread::yield();
}

void startWorkers(std::vector<std::thread>& workers)
{
    for (int worker = 0; worker < kWorkers; ++worker)
        workers.emplace_back(work, worker);
    while (gWaiting.load() != kWorkers)
        std::this_thread::yield();
}

void joinWorkers(std::vector<std::thread>& workers)
{
    for (std::thread& worker : workers)
        worker.join();
}

// The transition goes on after the pause, the released workers logging meanwhile
void releaseWorkers()
{
    if (gReleased.exchange(true))
        return;
    const auto end = std::chrono::steady_clock::now() + gPause;
    while (std::chrono::steady_clock::now() < end)
        continue;
}

[[noreturn]] void waitForTheCrash()
{
    while (true)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

// The scenario's part at its point: the thread there crashes, or the workers go, the crashing one first among them
void atPoint(Point point)
{
    if (point != gScenario->releaseAt)
        return;
    if (crashes(*gScenario) && !gScenario->onWorker)
        crash(gCrash, pointLogs(point) ? CrashLine::Log : CrashLine::None);
    releaseWorkers();
    // The exit holds none of the logger's locks at its points, so it stays there until the worker's crash ends it
    if (gScenario->onWorker && gScenario->phase == Phase::End)
        waitForTheCrash();
}

// SyncLogLocking, POSIX's alone, is no scenario's point
Point pointOf(LogTransition transition)
{
    switch (transition)
    {
    case LogTransition::SyncLogOpen:
        return Point::SyncLogOpen;
    case LogTransition::AsyncStart:
        return Point::AsyncStart;
    case LogTransition::AsyncRunning:
        return Point::AsyncRunning;
    case LogTransition::AsyncPublished:
        return Point::AsyncPublished;
    case LogTransition::AsyncFirstRead:
        return Point::AsyncFirstRead;
    case LogTransition::AsyncEnd:
        return Point::AsyncEnd;
    case LogTransition::AsyncEndWaited:
        return Point::AsyncEndWaited;
    case LogTransition::AsyncEnded:
        return Point::AsyncEnded;
    case LogTransition::SyncLogClose:
        return Point::SyncLogClose;
    case LogTransition::SyncLogClosed:
        return Point::SyncLogClosed;
    case LogTransition::SyncLogLocking:
        break;
    }
    return Point::Main;
}

void onTransition(LogTransition transition)
{
    if (transition != LogTransition::SyncLogLocking)
        atPoint(pointOf(transition));
}

// Anything on stderr fails the run
void releaseUnlessReleased()
{
    if (gReleased.load())
        return;
    fputs("the workers were not released at the transition\n", stderr);
    releaseWorkers();
}

// Each pair goes to another logger than the pair before: none carries a repeat across a hand-over
void logMainAgain()
{
    LLOG("stress main again");
    LLOG("stress main again");
}

struct ReleasesWorkers
{
    ~ReleasesWorkers()
    {
        releaseWorkers();
    }
};

// Destroyed after the async logger's end, so the main thread logs synchronously
struct JoinsWorkers
{
    std::vector<std::thread> workers;

    ~JoinsWorkers()
    {
        // A point after this level is the exiting thread's alone
        if (afterTheWorkers(gScenario->releaseAt))
            releaseWorkers();
        else
            releaseUnlessReleased();
        gStopped.store(true);
        joinWorkers(workers);
        logMainAgain();
        LOG("stress joined");
    }
};

// The transition comes with the main thread's first LLOG or LOG
void runStart(Phase phase)
{
    if (phase == Phase::Start)
        logMainAgain();
    std::vector<std::thread> workers;
    startWorkers(workers);
    if (phase == Phase::Open)
    {
        LLOG("stress main first");
    }
    else
    {
        LOG("stress main first");
        logMainAgain();
    }
    // The logger thread makes that transition on its own, soon after the start
    while (gScenario->releaseAt == Point::AsyncFirstRead && !gReleased.load())
        std::this_thread::yield();
    releaseUnlessReleased();
    joinWorkers(workers);
}

void logMainFirst()
{
    LOG("stress main first");
}

// Teardown's main lines come from a joined thread, so that no Quill context is left on main when its leak check runs
void logMainLines(Phase phase, void (*lines)())
{
    if (phase == Phase::Teardown)
        std::thread(lines).join();
    else
        lines();
}

// Logs at its point of the exit, then does the scenario's part there
struct PointMarker
{
    Point point;
    const char* text;

    PointMarker(Point at, const char* line)
        : point(at)
        , text(line)
    {
    }

    ~PointMarker()
    {
        LOG("{}", text);
        atPoint(point);
    }
};

// The exit's points End marks: a static gone before DestructionManager, above the logger, below it, after the close
void markExitPoints()
{
    static PointMarker exitStart(Point::ExitStart, "stress at the exit's start");
    DestructionManager::make_shared<PointMarker>(50, Point::DestructionAbove, "stress above the logger");
    DestructionManager::make_shared<PointMarker>(7, Point::DestructionBelow, "stress below the logger");
    DestructionManager::make_shared<PointMarker>(0, Point::AfterClose, "stress after the close");
}

// The transition comes in the teardown, the workers logging through it until JoinsWorkers stops them, or for good
void runEnd(const Scenario& scenario)
{
    const Phase phase = scenario.phase;
    logMainLines(phase, logMainAgain);
    // The async logger runs before the workers log, so it ends holding their queues and counts
    logMainLines(phase, logMainFirst);
    if (phase == Phase::Teardown)
        DestructionManager::make_shared<ReleasesWorkers>(50);
    if (scenario.abandon)
    {
        memset(gTailPadding, 't', sizeof(gTailPadding));
        std::vector<std::thread> workers;
        startWorkers(workers);
        for (std::thread& worker : workers)
            worker.detach();
    }
    else
    {
        JoinsWorkers* joiner = DestructionManager::make_shared<JoinsWorkers>(5).get();
        startWorkers(joiner->workers);
    }
    logMainLines(phase, logMainAgain);
    if (phase == Phase::Teardown)
        DestructionManager::destroyNow();
}

// The main thread or a worker crashes while the workers log; the logger's crash hooks write what came before it
void runCrash(const Scenario& scenario)
{
    logMainAgain();
    LOG("stress main first");
    std::vector<std::thread> workers;
    startWorkers(workers);
    logMainAgain();
    releaseWorkers();
    if (!scenario.onWorker)
        crash(gCrash, CrashLine::Log);
    joinWorkers(workers);
}

// A crash or a failed assertion ends the child with its code and the text on stderr, never waits on a dialog
void reportFaultsOnStderr()
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
#ifdef _MSC_VER
    // DebugTools' crash filter writes its dump inside the crashing process, which a thread in the loader can hang
    SetUnhandledExceptionFilter(nullptr);
#endif
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
#endif
}

#if HAVE_LEAKFINDER
// At level 3, after the logger's end and before the leak finder's: what the child still holds of its allocations leaks
struct LeakCheck
{
    DebugTools::MemorySnapshot since = DebugTools::kNoSnapshot;

    ~LeakCheck()
    {
        const DebugTools::LeakFinderState state = DebugTools::leakfinder_get_state(since, DebugTools::kNoSnapshot);
        if (state.leaks_count == 0)
            return;
        fprintf(stderr, "%zu leaks, %zu bytes\n", state.leaks_count, state.leaks_bytes);
        DebugTools::leakfinder_dump(since, DebugTools::kNoSnapshot); // their stacks, to a debugger's output
    }
};
#endif

int runChild(const Scenario& scenario, uint64_t seed, const char* logFile)
{
    // Before the leak check's start, as they outlive its level
    if (scenario.phase == Phase::End)
        markExitPoints();
#if HAVE_LEAKFINDER
    // A crash leaves the heap as it was, and abandoned workers hold theirs
    if (!crashes(scenario) && !scenario.abandon)
    {
        const std::shared_ptr<LeakCheck> check = DestructionManager::make_shared<LeakCheck>(3);
        check->since = DebugTools::leakfinder_snapshot();
    }
#endif
    reportFaultsOnStderr();
    setenv("LOG_FILE", logFile, 1);
    // 128 times smaller than the logger's own, so the queues grow, fill and get recycled often
    setLogQueueCapacity(2 * 1024, 16 * 1024);
    static constexpr int kPausesUs[] = {0, 20, 100, 500, 2000};
    rand_default_t random;
    srand64_r(random, seed);
    gPause = std::chrono::microseconds(kPausesUs[rand64_r(random) % std::size(kPausesUs)]);
    gCrashAt = static_cast<int>(rand64_r(random) % workerPlan(Phase::Crash, kCrashingWorker).after);
    // A worker crashing at a point does it as it is released there
    if (scenario.phase != Phase::Crash)
        gCrashAt = 0;
    gNowhere = reinterpret_cast<volatile int*>(static_cast<uintptr_t>(rand64_r(random) % 4096) & ~uintptr_t(3));
    gCrash = crashOf(scenario, seed);
    gScenario = &scenario;
    gSeed = seed;
    if (scenario.phase != Phase::Teardown && scenario.phase != Phase::Crash)
        setLogTransitionHook(onTransition);
    if (scenario.phase == Phase::Open || scenario.phase == Phase::Start)
        runStart(scenario.phase);
    else if (scenario.phase == Phase::Crash)
        runCrash(scenario);
    else
        runEnd(scenario);
    return 0;
}

fs::path executablePath()
{
#ifdef _WIN32
    wchar_t path[4 * MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return fs::path(std::wstring(path, length));
#elif defined(__APPLE__)
    char path[4096];
    uint32_t size = sizeof(path);
    return _NSGetExecutablePath(path, &size) == 0 ? fs::path(path) : fs::path();
#else
    std::error_code error;
    return fs::read_symlink("/proc/self/exe", error);
#endif
}

int gHangSeconds = 60; // a run longer than this is hung: its dump is written and it is ended; 0: no limit
bool gAttach = false;  // a hung run is left as it is for a debugger until Enter
constexpr long long kHung = -2;
#ifndef _WIN32
constexpr long long kSignaled = 0x100; // a signal's end, apart from every exit status
#endif

// How a child ended, as waitChild gives it
std::string endText(long long code)
{
#ifndef _WIN32
    if (code >= kSignaled)
        return fmtStr("ended by signal %lld", code - kSignaled);
#endif
    if (code > 255)
        return fmtStr("exited with 0x%llx", static_cast<unsigned long long>(code));
    return fmtStr("exited with %lld", code);
}

void waitForDebugger(long long pid)
{
    printf("  process %lld did not end within %d s and is left as it is: attach a debugger (cdb -p %lld, gdb -p %lld), "
           "then press Enter to end it\n",
           pid, gHangSeconds, pid, pid);
    fflush(stdout);
    for (int c = getchar(); c != '\n' && c != EOF; c = getchar())
        continue;
}

#ifdef _WIN32
using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);

// The hung child's threads and memory, for a debugger later
void writeDump(HANDLE process, DWORD pid, const fs::path& dumpFile)
{
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    if (dbghelp == nullptr)
        return;
    const WriteDump write = std::bit_cast<WriteDump>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    if (write == nullptr)
        return;
    HANDLE file = CreateFileW(dumpFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    const int type = MiniDumpWithFullMemory | MiniDumpWithHandleData | MiniDumpWithThreadInfo;
    write(process, pid, file, static_cast<MINIDUMP_TYPE>(type), nullptr, nullptr, nullptr);
    CloseHandle(file);
}

long long waitChild(const PROCESS_INFORMATION& process, const fs::path& dumpFile)
{
    const DWORD limit = gHangSeconds > 0 ? static_cast<DWORD>(gHangSeconds) * 1000 : INFINITE;
    if (WaitForSingleObject(process.hProcess, limit) != WAIT_TIMEOUT)
    {
        DWORD code = 0;
        GetExitCodeProcess(process.hProcess, &code);
        return code;
    }
    writeDump(process.hProcess, process.dwProcessId, dumpFile);
    if (gAttach)
        waitForDebugger(process.dwProcessId);
    TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
    return kHung;
}
#else
// gdb's gcore, where it is installed, writes the hung child's core, for a debugger later
void writeDump(pid_t pid, const fs::path& dumpFile)
{
    const std::string prefix = dumpFile.string();
    const std::string pidText = std::to_string(pid);
    char* argv[] = {const_cast<char*>("gcore"), const_cast<char*>("-o"), const_cast<char*>(prefix.c_str()),
                    const_cast<char*>(pidText.c_str()), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // Its own process group: gcore is a script that runs gdb, and a timeout stops both
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t gcore = 0;
    const int error = posix_spawnp(&gcore, "gcore", &actions, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0)
        return;
    // A gcore that does not finish in time is stopped, and the hung child is ended all the same
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    pid_t ended = 0;
    while (ended == 0 && std::chrono::steady_clock::now() < deadline)
    {
        ended = waitpid(gcore, nullptr, WNOHANG);
        if (ended < 0 && errno == EINTR)
            ended = 0;
        if (ended == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (ended == 0)
    {
        kill(-gcore, SIGKILL);
        while (waitpid(gcore, nullptr, 0) < 0 && errno == EINTR)
            continue;
        return;
    }
    std::error_code ignored;
    fs::rename(prefix + "." + pidText, dumpFile, ignored);
}

long long waitChild(pid_t pid, const fs::path& dumpFile)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(gHangSeconds);
    int status = 0;
    while (true)
    {
        const pid_t ended = waitpid(pid, &status, gHangSeconds > 0 ? WNOHANG : 0);
        if (ended == pid)
            return WIFSIGNALED(status) ? kSignaled + WTERMSIG(status) : WEXITSTATUS(status);
        if (ended < 0 && errno != EINTR)
            return -1;
        if (ended == 0 && std::chrono::steady_clock::now() >= deadline)
            break;
        if (ended == 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    writeDump(pid, dumpFile);
    if (gAttach)
        waitForDebugger(pid);
    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        continue;
    return kHung;
}
#endif

// The child's exit code, -1 if it did not start or kHung; its output is dropped, its errors go to errorFile
long long spawnChild(const fs::path& self, const Scenario& scenario, uint64_t seed, const fs::path& logFile,
                     const fs::path& errorFile, const fs::path& dumpFile)
{
    const std::string seedText = std::to_string(seed);
#ifdef _WIN32
    std::wstring command = L"\"" + self.wstring() + L"\" --child " + fs::path(scenario.name).wstring() + L" " +
                           fs::path(seedText).wstring() + L" \"" + logFile.wstring() + L"\"";
    SECURITY_ATTRIBUTES inherited {sizeof(inherited), nullptr, TRUE};
    HANDLE output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING,
                                0, nullptr);
    HANDLE errors = CreateFileW(errorFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherited, CREATE_ALWAYS, 0,
                                nullptr);
    STARTUPINFOW startup {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = output;
    startup.hStdError = errors;
    PROCESS_INFORMATION process {};
    const BOOL started = CreateProcessW(self.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                                        &startup, &process);
    CloseHandle(output);
    CloseHandle(errors);
    if (!started)
        return -1;
    const long long code = waitChild(process, dumpFile);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
#else
    const std::string path = self.string();
    const std::string log = logFile.string();
    char* argv[] = {const_cast<char*>(path.c_str()), const_cast<char*>("--child"), const_cast<char*>(scenario.name),
                    const_cast<char*>(seedText.c_str()), const_cast<char*>(log.c_str()), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errorFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = 0;
    const int error = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error != 0)
        return -1;
    return waitChild(pid, dumpFile);
#endif
}

// A log line: after the date and time, the right-aligned thread id, the thread name column and the message
struct LogLine
{
    uint32_t threadId = 0;
    std::string_view name;
    std::string_view message;
};

LogLine parseLine(std::string_view line)
{
    LogLine parsed;
    size_t at = std::string_view("2026-10-14 23:41:07.123 ").size();
    while (at < line.size() && line[at] == ' ')
        ++at;
    if (at >= line.size())
        return parsed;
    const std::from_chars_result read = std::from_chars(line.data() + at, line.data() + line.size(), parsed.threadId);
    const size_t nameAt = static_cast<size_t>(read.ptr - line.data()) + 1;
    at = nameAt + kThreadNameWidth + 1;
    if (at > line.size())
        return parsed;
    parsed.name = line.substr(nameAt, kThreadNameWidth);
    while (!parsed.name.empty() && parsed.name.back() == ' ')
        parsed.name.remove_suffix(1);
    parsed.message = line.substr(at);
    return parsed;
}

bool skip(std::string_view& text, std::string_view prefix)
{
    if (!text.starts_with(prefix))
        return false;
    text.remove_prefix(prefix.size());
    return true;
}

bool readNumber(std::string_view& text, int& value)
{
    const std::from_chars_result read = std::from_chars(text.data(), text.data() + text.size(), value);
    if (read.ec != std::errc())
        return false;
    text.remove_prefix(static_cast<size_t>(read.ptr - text.data()));
    return true;
}

// A line a worker logs and the times it logs it in a row
struct Planned
{
    std::string text;
    int count;
    bool repeatsAsLines; // a long line: one over the queue's limit, or a synchronous LLOG's over 512 bytes, is not kept
    std::string name;    // the worker's name when it logs the line
};

// A worker's lines as drawIteration plans them, and how far the log has them
struct WorkerLog
{
    std::vector<Planned> planned;
    size_t beforeRelease = 0; // planned lines of the iterations before the release
    size_t next = 0;          // the planned line the log has next
    int counted = 0;          // the times the log has the one before
    std::vector<int> helperLines; // per helper thread, the lines the log has
    size_t helpersBeforeRelease = 0;
    bool handOverRepeat = false; // once, at the start: LLOG repeats the async logger takes over start again as a line

    size_t complete() const
    {
        return next == 0 || counted == planned[next - 1].count ? next : next - 1;
    }
};

WorkerLog planWorker(Phase phase, uint64_t seed, int worker)
{
    const WorkerPlan plan = workerPlan(phase, worker);
    rand_default_t random;
    srand64_r(random, workerSeed(seed, worker));
    WorkerLog log;
    log.handOverRepeat = phase == Phase::Start;
    std::string name = workerName(worker);
    for (int number = 0; number < plan.before + plan.after; ++number)
    {
        if (number == plan.before)
        {
            log.beforeRelease = log.planned.size();
            log.helpersBeforeRelease = log.helperLines.size();
        }
        const Iteration iteration = drawIteration(random, number < plan.before ? plan.llogBefore : plan.llogAfter);
        log.planned.push_back({numberedLine(worker, number, iteration), 1 + iteration.repeats,
                               iteration.kind == Kind::Long, name});
        if (iteration.literal)
            log.planned.push_back({"stress literal", 1 + iteration.literalRepeats, false, name});
        if (iteration.helper)
            log.helperLines.push_back(0);
        if (iteration.rename)
            name = renamedWorker(worker, number);
    }
    return log;
}

struct LogContent
{
    std::vector<WorkerLog> workers;
    std::unordered_map<uint32_t, int> workerOfThread; // from the thread's numbered lines; its literals have no number
    int mainFirst = 0;
    int mainAgainLines = 0;
    int mainAgainTotal = 0;
    int joined = 0;
    int crashes = 0;
    int runStarts = 0;
    MarkedLines marked {};
    bool tails = false; // abandoned workers' tail lines are allowed
};

const char* readHelperLine(std::string_view text, bool counted, WorkerLog& lines)
{
    int helper = 0;
    int line = 0;
    if (counted || !readNumber(text, helper) || !skip(text, " line ") || !readNumber(text, line) || !text.empty())
        return "an unexpected helper line";
    if (helper < 0 || static_cast<size_t>(helper) >= lines.helperLines.size() || lines.helperLines[helper] != line)
        return "a helper line out of order";
    ++lines.helperLines[helper];
    return nullptr;
}

// A worker's numbered line, its literal or a count of either, which must be the next of its planned lines
const char* readWorkerLine(std::string_view text, bool counted, int count, const LogLine& line, LogContent& log)
{
    const uint32_t threadId = line.threadId;
    int worker = -1;
    std::string_view rest = text;
    if (skip(rest, "stress ") && readNumber(rest, worker))
    {
        if (worker < 0 || worker >= kWorkers)
            return "an unexpected line";
        if (skip(rest, " helper "))
            return readHelperLine(rest, counted, log.workers[worker]);
        // An abandoned worker's line after its release, unchecked: the process ends it anywhere
        if (log.tails && rest.starts_with(" tail "))
            return nullptr;
        if (!rest.starts_with(" line "))
            return "an unexpected line";
    }
    else if (text != "stress literal")
        return "an unexpected line";
    const auto found = log.workerOfThread.find(threadId);
    if (worker < 0 && found == log.workerOfThread.end())
        return "a literal before its thread's first line";
    if (worker < 0)
        worker = found->second;
    else if (found == log.workerOfThread.end())
        log.workerOfThread[threadId] = worker;
    else if (found->second != worker)
        return "two workers' lines on one thread";
    WorkerLog& lines = log.workers[worker];
    if (counted)
    {
        if (lines.next == 0 || lines.planned[lines.next - 1].text != text ||
            lines.counted + count > lines.planned[lines.next - 1].count)
        {
            return "a count out of order";
        }
        lines.counted += count;
        return nullptr;
    }
    const bool repeat = lines.next > 0 && lines.planned[lines.next - 1].text == text &&
                        lines.counted < lines.planned[lines.next - 1].count;
    if (repeat && (lines.planned[lines.next - 1].repeatsAsLines || lines.handOverRepeat))
    {
        lines.handOverRepeat = lines.handOverRepeat && lines.planned[lines.next - 1].repeatsAsLines;
        ++lines.counted;
        return line.name != lines.planned[lines.next - 1].name ? "a line under another name" : nullptr;
    }
    if (lines.next == lines.planned.size() || lines.planned[lines.next].text != text || lines.complete() != lines.next)
        return "a line out of order";
    ++lines.next;
    lines.counted = 1;
    return line.name != lines.planned[lines.next - 1].name ? "a line under another name" : nullptr;
}

// What is wrong with the line coming next, if anything
const char* readMessage(const LogLine& line, LogContent& log)
{
    std::string_view text = line.message;
    int count = 1;
    const bool counted = skip(text, "repeated ");
    if (counted)
    {
        if (!readNumber(text, count) || !skip(text, count == 1 ? " time: " : " times: "))
            return "a malformed count";
        text = text.substr(0, text.find(" (the last "));
    }
    if (text == "stress main again")
    {
        log.mainAgainLines += counted ? 0 : 1;
        log.mainAgainTotal += count;
        return nullptr;
    }
    if (counted)
        return readWorkerLine(text, counted, count, line, log);
    if (text == "stress main first")
    {
        ++log.mainFirst;
        return nullptr;
    }
    if (text == "stress joined")
    {
        ++log.joined;
        return nullptr;
    }
    if (text == "stress crash")
    {
        ++log.crashes;
        return nullptr;
    }
    if (text == "stress at the exit's start")
    {
        ++log.marked.exitStart;
        return nullptr;
    }
    if (text == "stress above the logger")
    {
        ++log.marked.above;
        return nullptr;
    }
    if (text == "stress below the logger")
    {
        ++log.marked.below;
        return nullptr;
    }
    if (text == "stress after the close")
    {
        ++log.marked.afterClose;
        return nullptr;
    }
    if (text.starts_with("threads still holding a log queue at the logger's end: "))
        return nullptr;
    if (text.starts_with("==== log start, pid "))
    {
        ++log.runStarts;
        return nullptr;
    }
    if (text.find(" Quill INFO: Allocated a new SPSC queue with a capacity of ") != std::string_view::npos)
        return nullptr;
    return readWorkerLine(text, counted, count, line, log);
}

// A crash that loses lines: whatever the log holds is in order, any count of each line up to the planned one
std::string checkCut(const Scenario& scenario, const LogContent& log)
{
    const MainLines main = mainLines(scenario);
    const MarkedLines marked = markedLines(scenario);
    if (log.runStarts > 1 || log.mainFirst > 1 || log.crashes > 1 || log.mainAgainTotal > main.againTotal ||
        log.joined > main.joined || log.marked.exitStart > marked.exitStart || log.marked.above > marked.above ||
        log.marked.below > marked.below || log.marked.afterClose > marked.afterClose)
    {
        return fmtStr("more lines than planned: %d \"log start\", %d \"main first\", %d \"stress crash\", %d \"main "
                      "again\", %d \"joined\", %d, %d, %d and %d at the exit's points",
                      log.runStarts, log.mainFirst, log.crashes, log.mainAgainTotal, log.joined, log.marked.exitStart,
                      log.marked.above, log.marked.below, log.marked.afterClose);
    }
    return {};
}

std::string checkLog(const Scenario& scenario, uint64_t seed, const std::string& text)
{
    LogContent log;
    log.tails = scenario.abandon;
    for (int worker = 0; worker < kWorkers; ++worker)
        log.workers.push_back(planWorker(scenario.phase, seed, worker));
    size_t number = 0;
    for (size_t begin = 0; begin < text.size(); ++number)
    {
        const size_t end = text.find('\n', begin);
        // A crash can end the process while another thread is writing
        if (end == std::string::npos && crashes(scenario))
            break;
        if (end == std::string::npos)
            return "the log's last line has no end";
        const std::string_view line(text.data() + begin, end - begin);
        if (const char* problem = readMessage(parseLine(line), log))
        {
            const int shown = static_cast<int>(line.size() < 300 ? line.size() : 300);
            return fmtStr("log line %zu, \"%.*s\": %s", number + 1, shown, line.data(), problem);
        }
        begin = end + 1;
    }
    if (crashes(scenario) && !crashKeepsLines(scenario))
        return checkCut(scenario, log);
    if (log.runStarts != 1)
        return fmtStr("%d \"log start\" lines", log.runStarts);
    if (log.mainFirst != 1)
        return fmtStr("%d \"stress main first\" lines", log.mainFirst);
    if (crashes(scenario))
    {
        if (log.crashes != keptCrashLines(scenario))
            return fmtStr("%d \"stress crash\" lines, expected %d", log.crashes, keptCrashLines(scenario));
        if (const std::string problem = checkCut(scenario, log); !problem.empty())
            return problem;
    }
    // A crash in main, or after JoinsWorkers' level, comes after all of main's lines
    if (!crashes(scenario) || scenario.phase == Phase::Crash || afterTheWorkers(scenario.releaseAt))
    {
        const MainLines expected = mainLines(scenario);
        if (log.mainAgainLines < expected.againLinesMin || log.mainAgainLines > expected.againLinesMax ||
            log.mainAgainTotal != expected.againTotal)
        {
            return fmtStr("\"stress main again\": %d lines, %d with their repeats; expected %d to %d and %d",
                          log.mainAgainLines, log.mainAgainTotal, expected.againLinesMin, expected.againLinesMax,
                          expected.againTotal);
        }
        if (log.joined != expected.joined)
            return fmtStr("%d \"stress joined\" lines, expected %d", log.joined, expected.joined);
        if (!crashes(scenario) && log.crashes != 0)
            return fmtStr("%d \"stress crash\" lines", log.crashes);
        const MarkedLines marked = markedLines(scenario);
        if (log.marked.exitStart != marked.exitStart || log.marked.above != marked.above ||
            log.marked.below != marked.below || log.marked.afterClose != marked.afterClose)
        {
            return fmtStr("at the exit's start, above the logger, below it and after the close: %d, %d, %d and %d "
                          "lines, expected %d, %d, %d and %d",
                          log.marked.exitStart, log.marked.above, log.marked.below, log.marked.afterClose,
                          marked.exitStart, marked.above, marked.below, marked.afterClose);
        }
    }
    // Released after the logger's end, the workers' later lines are ignored, those of their helpers too
    const bool dropped = scenario.phase == Phase::End && pastTheEnd(scenario.releaseAt);
    for (int worker = 0; worker < kWorkers; ++worker)
    {
        const bool cut = workerPlan(scenario.phase, worker).cut;
        const WorkerLog& lines = log.workers[worker];
        if (lines.complete() < (cut ? lines.beforeRelease : lines.planned.size()) ||
            (dropped && lines.next > lines.beforeRelease))
        {
            return fmtStr("worker %d: %zu of its %zu lines, %zu of them before the release", worker, lines.complete(),
                          lines.planned.size(), lines.beforeRelease);
        }
        for (size_t helper = 0; helper < lines.helperLines.size(); ++helper)
        {
            const bool before = helper < lines.helpersBeforeRelease;
            if (((!cut || before) && lines.helperLines[helper] != kHelperLines) ||
                (dropped && !before && lines.helperLines[helper] != 0))
            {
                return fmtStr("worker %d helper %zu: %d of its lines", worker, helper, lines.helperLines[helper]);
            }
        }
    }
    return {};
}

std::string readText(const fs::path& file)
{
    std::string text;
    readTestFile(file.string(), text);
    return text;
}

// gcc's LeakSanitizer under load cannot read a thread that is just ending at exit: it says so, finds no leak, goes on
std::string withoutSanitizerNotes(const std::string& errors)
{
    std::string kept;
    for (size_t begin = 0; begin < errors.size();)
    {
        const size_t newline = errors.find('\n', begin);
        const size_t end = newline == std::string::npos ? errors.size() : newline + 1;
        const std::string_view line(errors.data() + begin, end - begin);
        if (line.find("Unable to get registers from thread") == std::string_view::npos)
            kept.append(line);
        begin = end;
    }
    return kept;
}

// A line of ASan's report of the planned fault, UBSan's without ASan, or TSan's, which stops after its first lines
bool sanitizerSegvLine(std::string_view line)
{
    if (line == "ThreadSanitizer: nested bug in the same thread, aborting.")
        return true;
    if (line.find("ERROR: ") != std::string_view::npos)
        return line.find("Sanitizer: SEGV on unknown address") != std::string_view::npos;
    // A fault on another thread than main adds where that thread was created
    const bool threadCreated = line.starts_with("Thread T") && line.find(" created by T") != std::string_view::npos;
    return line.starts_with("==") || line.starts_with("    #") || line.find(" = 0x") != std::string_view::npos ||
           line.ends_with("Sanitizer:DEADLYSIGNAL") || line.find("Sanitizer can not provide additional") != line.npos ||
           (line.starts_with("SUMMARY: ") && line.find("Sanitizer: SEGV") != line.npos) || threadCreated;
}

// UBSan without ASan has no macro to tell it by, but only it writes this
bool ubsanReported(const std::string& errors)
{
    return errors.find("ERROR: UndefinedBehaviorSanitizer: SEGV") != std::string::npos;
}

bool tsanReported(const std::string& errors)
{
    return errors.find("ERROR: ThreadSanitizer: SEGV") != std::string::npos;
}

// What the planned crash writes itself: the C++ runtime's words on the throw, MSVC Debug's on abort, sanitizer reports
bool plannedOutput(Crash crash, std::string_view line, bool sanitized)
{
    if (line.empty())
        return true;
    const bool uncaught = line == "terminate called after throwing an instance of 'std::runtime_error'" ||
                          line == "  what():  log_stress crash" ||
                          line == "libc++abi: terminating due to uncaught exception of type std::runtime_error: "
                                  "log_stress crash";
    if (crash == Crash::Throw && uncaught)
        return true;
    const bool terminated = line == "terminate called without an active exception" || line == "libc++abi: terminating";
    if (crash == Crash::Terminate && terminated)
        return true;
    if (crash != Crash::Fault && line == "abort() has been called")
        return true;
    // qemu's user-mode emulator, running another CPU's build, names the signal the planned crash ended the child with
    if (line == (crash == Crash::Fault ? "qemu: uncaught target signal 11 (Segmentation fault) - core dumped"
                                       : "qemu: uncaught target signal 6 (Aborted) - core dumped"))
        return true;
    return crash == Crash::Fault && sanitized && sanitizerSegvLine(line);
}

// Any other line in a crash run's errors is a failure of its own, as is a planned line written twice: a second crash
std::string unplannedOutput(const std::string& errors, Crash crash)
{
    std::string unplanned;
    std::vector<std::string_view> planned;
    const bool sanitized = HAVE_ASAN || ubsanReported(errors) || tsanReported(errors);
    for (size_t begin = 0; begin < errors.size();)
    {
        const size_t newline = errors.find('\n', begin);
        const size_t end = newline == std::string::npos ? errors.size() : newline + 1;
        std::string_view line(errors.data() + begin, end - begin);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);
        const bool again = !line.empty() && std::ranges::find(planned, line) != planned.end();
        if (again || !plannedOutput(crash, line, sanitized))
            unplanned.append(errors, begin, end - begin);
        else if (!line.empty())
            planned.push_back(line);
        begin = end;
    }
    return unplanned;
}

// The code the planned crash ends the child with, as waitChild gives it
bool plannedEnd(Crash crash, long long code, [[maybe_unused]] const std::string& errors)
{
#ifdef _WIN32
    if (crash == Crash::Fault)
        return code == static_cast<long long>(EXCEPTION_ACCESS_VIOLATION);
    // Abort, terminate and a throw's terminate end in abort: failing fast, from the log's handler or not, or with its 3
    if (code == 0xC0000409 || code == 3) // __fastfail's code
        return true;
    // On x86 an uncaught throw reaches no terminate: with no filter before the log's, SEH ends it with its own code
    return crash == Crash::Throw && code == 0xE06D7363;
#else
    // ASan, or UBSan without it, reports the fault the log's handler hands on to it and exits with 1, TSan with 66
    const bool asanReported = HAVE_ASAN && errors.find("ERROR: AddressSanitizer: SEGV") != std::string::npos;
    if (crash == Crash::Fault)
    {
        return code == kSignaled + SIGSEGV || (code == 1 && (asanReported || ubsanReported(errors))) ||
               (code == 66 && tsanReported(errors));
    }
    return code == kSignaled + SIGABRT;
#endif
}

std::string crashProblem(const Scenario& scenario, uint64_t seed, long long code, const std::string& errors,
                         const fs::path& logFile)
{
    const Crash crash = crashOf(scenario, seed);
    if (!plannedEnd(crash, code, errors))
        return endText(code) + ", not as the planned crash";
    if (const std::string unplanned = unplannedOutput(errors, crash); !unplanned.empty())
        return "wrote to stderr: " + unplanned;
    return checkLog(scenario, seed, readText(logFile));
}

// False at the scenario's first failed run, whose log stays for a look
bool runScenario(const fs::path& self, const Scenario& scenario, int runs)
{
    std::vector<double> times;
    for (int run = 0; run < runs; ++run)
    {
        const uint64_t seed = rand64() | 1;
        const fs::path logFile = fs::temp_directory_path() /
                                 fmtStr("log-stress-%u-%s-%d.txt", currentProcessId(), scenario.name, run);
        fs::path errorFile = logFile;
        errorFile += ".err";
        fs::path dumpFile = logFile;
        dumpFile += ".dmp";
        const long long started = timeMicro();
        const long long code = spawnChild(self, scenario, seed, logFile, errorFile, dumpFile);
        times.push_back(static_cast<double>(timeMicro() - started) / 1000);
        const std::string errors = withoutSanitizerNotes(readText(errorFile));
        std::string problem;
        if (code == -1)
            problem = "the child did not start";
        else if (code == kHung && fs::exists(dumpFile))
            problem = fmtStr("did not end within %d s; its dump: %s", gHangSeconds, dumpFile.string().c_str());
        else if (code == kHung)
            problem = fmtStr("did not end within %d s; no dump was written", gHangSeconds);
        else if (crashes(scenario))
            problem = crashProblem(scenario, seed, code, errors, logFile);
        else if (code != 0)
            problem = endText(code);
        else if (!errors.empty())
            problem = "wrote to stderr: " + errors;
        else
            problem = checkLog(scenario, seed, readText(logFile));
        if (!problem.empty())
        {
            printf("%s run %d failed: %s\n  rerun: \"%s\" --child %s %llu \"%s\"\n", scenario.name, run + 1,
                   problem.c_str(), self.string().c_str(), scenario.name, static_cast<unsigned long long>(seed),
                   logFile.string().c_str());
            fflush(stdout);
            return false;
        }
        std::error_code ignored;
        fs::remove(logFile, ignored);
        fs::remove(errorFile, ignored);
    }
    std::sort(times.begin(), times.end());
    printf("%-29s %d runs, %.1f to %.1f ms, median %.1f ms\n", scenario.name, runs, times.front(), times.back(),
           times[times.size() / 2]);
    fflush(stdout);
    return true;
}

int usage()
{
    fputs("usage: log_stress [--runs <n>] [--hang <seconds>] [--attach] [<scenario>...]\n"
          "       log_stress --child <scenario> <seed> <log file>\n"
          "--hang: a run longer than this (60 s; 0: no limit) has its dump written next to its log and is ended\n"
          "--attach: a run longer than that is left as it is for a debugger until Enter\n"
          "scenarios:",
          stderr);
    for (const Scenario& scenario : kScenarios)
        fprintf(stderr, " %s", scenario.name);
    fputs("\n", stderr);
    return 2;
}

// Cygwin ends a process that crashes inside exit() with exit()'s status, and the log's crash handlers write nothing
bool runsHere([[maybe_unused]] const Scenario& scenario)
{
#ifdef __CYGWIN__
    return !crashes(scenario) || scenario.phase != Phase::End;
#else
    return true;
#endif
}

int runParent(int argc, char** argv)
{
    int runs = 100;
    std::vector<const Scenario*> chosen;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
        {
            runs = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--hang") == 0 && i + 1 < argc)
        {
            gHangSeconds = atoi(argv[++i]);
            continue;
        }
        if (strcmp(argv[i], "--attach") == 0)
        {
            gAttach = true;
            continue;
        }
        const Scenario* scenario = findScenario(argv[i]);
        if (scenario == nullptr)
            return usage();
        chosen.push_back(scenario);
    }
    if (runs <= 0 || gHangSeconds < 0)
        return usage();
    if (chosen.empty())
    {
        for (const Scenario& scenario : kScenarios)
        {
            if (runsHere(scenario))
                chosen.push_back(&scenario);
        }
        if (chosen.size() != std::size(kScenarios))
            printf("%zu scenarios that crash inside exit() are skipped here\n", std::size(kScenarios) - chosen.size());
    }
    srand64(0);
    const fs::path self = executablePath();
    bool passed = true;
    for (const Scenario* scenario : chosen)
        passed = runScenario(self, *scenario, runs) && passed;
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--child") == 0)
    {
        const Scenario* scenario = argc == 5 ? findScenario(argv[2]) : nullptr;
        if (scenario == nullptr)
            return usage();
        return runChild(*scenario, strtoull(argv[3], nullptr, 10), argv[4]);
    }
    return runParent(argc, argv);
}
