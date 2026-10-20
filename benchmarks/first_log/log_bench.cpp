// log_bench: a log line's cost by statement and by way of logging it, while 1 or more processes log at once
#include "SyncLog.h"
#include "log.h"
#include "util.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#if defined(_WIN32) || defined(__CYGWIN__)
#include <windows.h>
#endif
#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

struct Point
{
    int x;
    int y;
};

template <>
struct std::formatter<Point> : std::formatter<int>
{
    auto format(const Point& point, std::format_context& context) const
    {
        return std::format_to(context.out(), "({}, {})", point.x, point.y);
    }
};

namespace
{

const std::string gText(80, 'x');
std::string gFormatted;
std::string gLine;
char gBuffer[256];
FILE* gPieceFile = nullptr;

// Two literals in turn, so no LOG repeats the one before it
void logLiteral(int i)
{
    if (i % 2 == 0)
        LOG("connection lost");
    else
        LOG("connection restored");
}

void logNumbers(int i)
{
    LOG("numbers {} {} {}", i, i * 7, i * 0.5);
}

void logPrintfNumbers(int i)
{
    LOG("numbers %d %d %f", i, i * 7, i * 0.5);
}

void logString(int i)
{
    LOG("string {} {}", i, gText);
}

void logPrintfString(int i)
{
    LOG("string %d %s", i, gText.c_str());
}

void logCustom(int i)
{
    LOG("custom {} {}", i, Point {i, -i});
}

void formatLiteral(int i)
{
    gFormatted = i % 2 == 0 ? "connection lost" : "connection restored";
}

void formatNumbers(int i)
{
    gFormatted = std::format("numbers {} {} {}", i, i * 7, i * 0.5);
}

void formatPrintfNumbers(int i)
{
    snprintf(gBuffer, sizeof(gBuffer), "numbers %d %d %f", i, i * 7, i * 0.5);
    gFormatted = gBuffer;
}

void formatString(int i)
{
    gFormatted = std::format("string {} {}", i, gText);
}

void formatPrintfString(int i)
{
    snprintf(gBuffer, sizeof(gBuffer), "string %d %s", i, gText.c_str());
    gFormatted = gBuffer;
}

void formatCustom(int i)
{
    gFormatted = std::format("custom {} {}", i, Point {i, -i});
}

using Call = void (*)(int);

template <Call Format>
void logFormatted(int i)
{
    Format(i);
    LOG(std::string_view(gFormatted));
}

template <Call Format, bool Flush>
void writeSync(int i)
{
    Format(i);
    gLine.assign(kLogPrefixSize, ' ');
    gLine += gFormatted;
    gLine += '\n';
    writeLogLine(gLine, ticksToTime(timeTicks()), 1234, "main", Flush);
}

void fileWrite(int)
{
    fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, gPieceFile);
}

void fileWriteFlush(int)
{
    fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, gPieceFile);
    fflush(gPieceFile);
}

void writeStdout(int)
{
    fwrite(gLine.data() + kLogPrefixSize, 1, gLine.size() - kLogPrefixSize, stdout);
}

void writeDebugger(int)
{
#if defined(_WIN32) || defined(__CYGWIN__)
    OutputDebugStringA(gLine.c_str() + kLogPrefixSize);
#endif
}

void noFlush()
{
}

void flushLog()
{
    LOG();
}

struct Statement
{
    const char* name;
    Call format;
    Call log;    // as written
    Call atCall; // formatted, then logged as text
    Call sync;
    Call syncFlushed;
};

const Statement kStatements[] = {
    {"1 literal", formatLiteral, logLiteral, logFormatted<formatLiteral>, writeSync<formatLiteral, false>,
     writeSync<formatLiteral, true>},
    {"2 {} numbers", formatNumbers, logNumbers, logFormatted<formatNumbers>, writeSync<formatNumbers, false>,
     writeSync<formatNumbers, true>},
    {"3 printf numbers", formatPrintfNumbers, logPrintfNumbers, logFormatted<formatPrintfNumbers>,
     writeSync<formatPrintfNumbers, false>, writeSync<formatPrintfNumbers, true>},
    {"4 {} string", formatString, logString, logFormatted<formatString>, writeSync<formatString, false>,
     writeSync<formatString, true>},
    {"5 printf string", formatPrintfString, logPrintfString, logFormatted<formatPrintfString>,
     writeSync<formatPrintfString, false>, writeSync<formatPrintfString, true>},
    {"6 custom type", formatCustom, logCustom, logCustom, writeSync<formatCustom, false>,
     writeSync<formatCustom, true>},
};

constexpr int kStatementCount = static_cast<int>(std::size(kStatements));
constexpr int kColumns = 5; // LOG, formatted at the call, formatting alone, synchronous, synchronous and flushed
constexpr int kPieces = 4;  // file, file and flush, stdout, debugger
constexpr int kPiecesAt = kStatementCount * kColumns;
constexpr int kLoggerAt = kPiecesAt + kPieces; // the logger thread's time a line
constexpr int kValues = kLoggerAt + 1;

using Values = std::array<double, kValues>;

// Median over batches of ns a call; the flush between batches is not timed
double medianNs(Call call, void (*flush)(), int batches, int perBatch)
{
    std::vector<double> results;
    for (int batch = 0; batch < batches; ++batch)
    {
        const long long start = tsc::timeTicks();
        for (int i = 0; i < perBatch; ++i)
            call(i);
        results.push_back(static_cast<double>(tsc::ticksToNano(tsc::timeTicks() - start)) / perBatch);
        flush();
    }
    std::sort(results.begin(), results.end());
    return results[results.size() / 2];
}

// From a burst of queued lines to LOG() returning once the logger thread wrote them, the median ns a line
double loggerNs()
{
    constexpr int kLines = 2000;
    std::vector<double> results;
    for (int burst = 0; burst < 20; ++burst)
    {
        const long long start = tsc::timeTicks();
        for (int i = 0; i < kLines; ++i)
            logNumbers(i);
        LOG();
        results.push_back(static_cast<double>(tsc::ticksToNano(tsc::timeTicks() - start)) / kLines);
    }
    std::sort(results.begin(), results.end());
    return results[results.size() / 2];
}

// Waits for the go file, so the processes of a run measure together; the values go to stderr on one line
int runChild(const std::string& name, const std::string& go)
{
    setenv("LOG_FILE", (name + ".log").c_str(), 1);
    while (!fs::exists(go))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Values values {};
    for (int s = 0; s < kStatementCount; ++s)
    {
        values[s * kColumns + 2] = medianNs(kStatements[s].format, noFlush, 50, 1000);
        values[s * kColumns + 3] = medianNs(kStatements[s].sync, noFlush, 30, 100);
        values[s * kColumns + 4] = medianNs(kStatements[s].syncFlushed, noFlush, 30, 100);
    }
    gPieceFile = fopen((name + ".pieces").c_str(), "wb");
    if (gPieceFile == nullptr)
    {
        fprintf(stderr, "cannot open %s.pieces\n", name.c_str());
        return 1;
    }
    writeSync<formatString, false>(0);
    values[kPiecesAt] = medianNs(fileWrite, noFlush, 30, 100);
    values[kPiecesAt + 1] = medianNs(fileWriteFlush, noFlush, 30, 100);
    values[kPiecesAt + 2] = medianNs(writeStdout, noFlush, 30, 100);
    values[kPiecesAt + 3] = medianNs(writeDebugger, noFlush, 30, 100);
    fclose(gPieceFile);
    LOG("start");
    LOG();
    for (int s = 0; s < kStatementCount; ++s)
    {
        values[s * kColumns] = medianNs(kStatements[s].log, flushLog, 30, 200);
        values[s * kColumns + 1] = medianNs(kStatements[s].atCall, flushLog, 30, 200);
    }
    values[kLoggerAt] = loggerNs();
    fputs("values", stderr);
    for (double value : values)
        fprintf(stderr, " %.2f", value);
    fputs("\n", stderr);
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

struct Child
{
    std::string name;
#ifdef _WIN32
    HANDLE process = nullptr;
#else
    pid_t pid = 0;
#endif
};

// Started in the current directory; its output is dropped, its errors and values go to <name>.err
bool startChild(const fs::path& self, const std::string& go, Child& child)
{
    const std::string errorFile = child.name + ".err";
#ifdef _WIN32
    std::wstring command = L"\"" + self.wstring() + L"\" --child " + fs::path(child.name).wstring() + L" " +
                           fs::path(go).wstring();
    SECURITY_ATTRIBUTES inherited {sizeof(inherited), nullptr, TRUE};
    HANDLE output = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited, OPEN_EXISTING,
                                0, nullptr);
    HANDLE errors = CreateFileW(fs::path(errorFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherited,
                                CREATE_ALWAYS, 0, nullptr);
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
        return false;
    CloseHandle(process.hThread);
    child.process = process.hProcess;
    return true;
#else
    const std::string path = self.string();
    char* argv[] = {const_cast<char*>(path.c_str()), const_cast<char*>("--child"),
                    const_cast<char*>(child.name.c_str()), const_cast<char*>(go.c_str()), nullptr};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, errorFile.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    const int error = posix_spawn(&child.pid, path.c_str(), &actions, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    return error == 0;
#endif
}

long long waitChild(const Child& child)
{
#ifdef _WIN32
    WaitForSingleObject(child.process, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(child.process, &code);
    CloseHandle(child.process);
    return code;
#else
    int status = 0;
    while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR)
        continue;
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : WEXITSTATUS(status);
#endif
}

// The child's values from its errors file; false, with the problem printed, if it wrote anything else
bool readValues(const Child& child, long long code, Values& values)
{
    std::string text;
    readFile(child.name + ".err", text);
    const char* at = text.c_str();
    bool read = code == 0 && strncmp(at, "values", 6) == 0;
    at += read ? 6 : 0;
    for (int i = 0; read && i < kValues; ++i)
    {
        char* end = nullptr;
        values[i] = strtod(at, &end);
        read = end != at;
        at = end;
    }
    while (*at == '\r' || *at == '\n')
        ++at;
    if (read && *at == '\0')
        return true;
    printf("process %s exited with %lld and wrote: %s\n", child.name.c_str(), code, text.c_str());
    return false;
}

struct Spread
{
    double median;
    double largest;
};

Spread spreadOf(const std::vector<Values>& results, int value)
{
    std::vector<double> values;
    for (const Values& result : results)
        values.push_back(result[value]);
    std::sort(values.begin(), values.end());
    return {values[values.size() / 2], values.back()};
}

void printResults(int processes, const std::vector<Values>& results)
{
    printf("\n%d process%s logging at once, ns a line, the median of the processes (stdout to the null device):\n",
           processes, processes == 1 ? "" : "es");
    printf("%-18s %8s %8s %8s %8s %11s\n", "statement", "LOG", "at call", "format", "sync", "sync+flush");
    for (int s = 0; s < kStatementCount; ++s)
    {
        const int at = s * kColumns;
        printf("%-18s %8.1f %8.1f %8.0f %8.0f %11.0f\n", kStatements[s].name, spreadOf(results, at).median,
               spreadOf(results, at + 1).median, spreadOf(results, at + 2).median, spreadOf(results, at + 3).median,
               spreadOf(results, at + 4).median);
    }
    const Spread debugger = spreadOf(results, kPiecesAt + 3);
    printf("pieces: file %.0f, file and flush %.0f, stdout %.0f, debugger %.0f (the slowest process %.0f)\n",
           spreadOf(results, kPiecesAt).median, spreadOf(results, kPiecesAt + 1).median,
           spreadOf(results, kPiecesAt + 2).median, debugger.median, debugger.largest);
    const Spread logger = spreadOf(results, kLoggerAt);
    printf("logger thread: %.0f a line (the slowest process %.0f)\n", logger.median, logger.largest);
    fflush(stdout);
}

void removeFiles(const std::vector<Child>& children, const std::string& go)
{
    std::error_code ignored;
    for (const Child& child : children)
    {
        fs::remove(child.name + ".log", ignored);
        fs::remove(child.name + ".pieces", ignored);
        fs::remove(child.name + ".err", ignored);
    }
    fs::remove(go, ignored);
}

// In the current directory; the files of a failed run stay for a look
bool runProcesses(const fs::path& self, int processes)
{
    const std::string go = fmtStr("go-%d", processes);
    std::vector<Child> children(static_cast<size_t>(processes));
    for (int i = 0; i < processes; ++i)
    {
        children[i].name = fmtStr("%d-%d", processes, i);
        if (!startChild(self, go, children[i]))
        {
            printf("process %s did not start\n", children[i].name.c_str());
            return false;
        }
    }
    FILE* goFile = fopen(go.c_str(), "wb");
    if (goFile != nullptr)
        fclose(goFile);
    std::vector<Values> results(children.size());
    bool passed = goFile != nullptr;
    for (size_t i = 0; i < children.size(); ++i)
        passed = readValues(children[i], waitChild(children[i]), results[i]) && passed;
    if (!passed)
        return false;
    printResults(processes, results);
    removeFiles(children, go);
    return true;
}

int usage()
{
    fputs("usage: log_bench [<processes>...]   (default: 1 2 4 8 16)\n"
          "       log_bench --child <name> <go file>\n",
          stderr);
    return 2;
}

int runParent(int argc, char** argv)
{
    std::vector<int> counts;
    for (int i = 1; i < argc; ++i)
    {
        const int count = atoi(argv[i]);
        if (count <= 0)
            return usage();
        counts.push_back(count);
    }
    if (counts.empty())
        counts = {1, 2, 4, 8, 16};
    const fs::path self = executablePath();
    const fs::path home = fs::current_path();
    const fs::path work = fs::temp_directory_path() / fmtStr("log-bench-%u", currentProcessId());
    fs::create_directories(work);
    // The children start here, so their files have plain relative names
    fs::current_path(work);
    bool passed = true;
    for (int count : counts)
        passed = passed && runProcesses(self, count);
    fs::current_path(home);
    std::error_code ignored;
    if (passed)
        fs::remove(work, ignored);
    else
        printf("files in %s\n", work.string().c_str());
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc > 1 && strcmp(argv[1], "--child") == 0)
    {
        if (argc != 4)
            return usage();
        return runChild(argv[2], argv[3]);
    }
    return runParent(argc, argv);
}
