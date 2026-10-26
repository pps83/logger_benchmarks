// clock_drift [seconds] [first steered round]: each clock's rate against each NTP server's address in ppm, + is slow
#include "sysutil.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
using Socket = int;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#ifdef __linux__
#include <sys/timex.h>
#endif

static const char* const kServers[] = {
    "ntp.ix.ru", "pool.ntp.org", "time.nist.gov", "time.apple.com", "time.google.com", "time.windows.com",
    "time.cloudflare.com", "time.facebook.com", "ntp.nict.jp", "time.stdtime.gov.tw", "ntp.aliyun.com",
    "time.nplindia.org", "ntp1.vniiftri.ru", "ptbtime1.ptb.de", "a.st1.ntp.br",
};

#ifdef _WIN32
enum Clock { kBte, kTsc, kSystem, kSteady, kHighRes, kInterrupt, kUnbiased, kSteered, kClocks };
static const char* const kClockNames[kClocks] = {"bte timeTicks", "bte tsc", "system", "steady", "high_resolution",
                                                 "QueryInterruptTimePrecise", "QueryUnbiasedInterruptTimePrecise",
                                                 "bte steered"};

using InterruptTimeFunc = void(WINAPI*)(PULONGLONG);

static InterruptTimeFunc interruptTimeFunc(const char* name)
{
    return reinterpret_cast<InterruptTimeFunc>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), name));
}

// 100 ns units, or 0 where Windows has no such function
static int64_t interruptNs(InterruptTimeFunc func)
{
    ULONGLONG t = 0;
    if (func)
        func(&t);
    return int64_t(t) * 100;
}
#else
#ifdef __APPLE__
#define CLOCK_EXTRA CLOCK_UPTIME_RAW
#define CLOCK_EXTRA_NAME "CLOCK_UPTIME_RAW"
#else
#define CLOCK_EXTRA CLOCK_BOOTTIME
#define CLOCK_EXTRA_NAME "CLOCK_BOOTTIME"
#endif
enum Clock { kBte, kTsc, kSystem, kSteady, kHighRes, kMonotonic, kMonotonicRaw, kExtra, kSteered, kClocks };
static const char* const kClockNames[kClocks] = {"bte timeTicks", "bte tsc", "system", "steady", "high_resolution",
                                                 "CLOCK_MONOTONIC", "CLOCK_MONOTONIC_RAW", CLOCK_EXTRA_NAME,
                                                 "bte steered"};

static int64_t clockNs(clockid_t id)
{
    timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
#endif

struct Clocks
{
    int64_t ns[kClocks];
};

// From the first steered round on, bte's clock at NTP's rate, set each round; never stepped, so it stays monotonic
struct Steering
{
    int64_t fromBte = 0; // bte's clock in ns where the current rate starts
    int64_t fromNs = 0;  // the steered clock there
    double rate = 1;     // steered ns per bte ns
};

static Steering gSteering;

static int64_t steeredNs(int64_t bteNs)
{
    return gSteering.fromNs + static_cast<int64_t>(static_cast<double>(bteNs - gSteering.fromBte) * gSteering.rate);
}

static Clocks readClocks()
{
    Clocks c;
    c.ns[kBte] = ticksToNano(timeTicks());
    c.ns[kSteered] = steeredNs(c.ns[kBte]);
    c.ns[kTsc] = tsc::ticksToNano(tsc::timeTicks());
    c.ns[kSystem] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    c.ns[kSteady] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    c.ns[kHighRes] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::high_resolution_clock::now().time_since_epoch()).count();
#ifdef _WIN32
    static const InterruptTimeFunc interrupt = interruptTimeFunc("QueryInterruptTimePrecise");
    static const InterruptTimeFunc unbiased = interruptTimeFunc("QueryUnbiasedInterruptTimePrecise");
    c.ns[kInterrupt] = interruptNs(interrupt);
    c.ns[kUnbiased] = interruptNs(unbiased);
#else
    c.ns[kMonotonic] = clockNs(CLOCK_MONOTONIC);
    c.ns[kMonotonicRaw] = clockNs(CLOCK_MONOTONIC_RAW);
    c.ns[kExtra] = clockNs(CLOCK_EXTRA);
#endif
    return c;
}

struct Sample
{
    int64_t at;     // steady clock, ns since the start
    int64_t rtt;    // ns, the server's own time taken off
    int64_t offset[kClocks];
    int stratum;
    int round;
};

struct Peer
{
    std::string server;
    std::string address;
    sockaddr_in addr;
    std::vector<Sample> samples;
};

// NTP's 64-bit timestamp at `p` as ns since 1970
static int64_t ntpNs(const unsigned char* p)
{
    const uint64_t sec = (uint64_t(p[0]) << 24) | (uint64_t(p[1]) << 16) | (uint64_t(p[2]) << 8) | p[3];
    const uint64_t frac = (uint64_t(p[4]) << 24) | (uint64_t(p[5]) << 16) | (uint64_t(p[6]) << 8) | p[7];
    return int64_t(sec - 2208988800ull) * 1000000000 + int64_t((frac * 1000000000) >> 32);
}

static bool queryOnce(const Peer& peer, int64_t start, Sample& sample)
{
    const int size = 48;
    const Socket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return false;
#ifdef _WIN32
    DWORD timeout = 500;
#else
    timeval timeout = {0, 500000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    unsigned char packet[size] = {0x23};
    const Clocks t1 = readClocks();
    bool ok = sendto(s, reinterpret_cast<const char*>(packet), size, 0, reinterpret_cast<const sockaddr*>(&peer.addr),
                     sizeof(peer.addr)) == size;
    ok = ok && recv(s, reinterpret_cast<char*>(packet), size, 0) == size;
    const Clocks t4 = readClocks();
    closesocket(s);
    if (!ok || (packet[0] & 7) != 4 || packet[1] == 0)
        return false;
    const int64_t t2 = ntpNs(packet + 32), t3 = ntpNs(packet + 40);
    sample.at = t1.ns[kSteady] - start;
    sample.rtt = (t4.ns[kSteady] - t1.ns[kSteady]) - (t3 - t2);
    sample.stratum = packet[1];
    for (int k = 0; k < kClocks; ++k)
        sample.offset[k] = ((t2 - t1.ns[k]) + (t3 - t4.ns[k])) / 2;
    return true;
}

// ppm with 3 decimals as an integer: the offset's change in ns over the elapsed ns, times 1e9
static int64_t ppmMilli(const Sample& first, const Sample& last, int k)
{
    return (last.offset[k] - first.offset[k]) * 1000000000 / (last.at - first.at);
}

static void printPpm(FILE* f, int64_t milli)
{
    fprintf(f, "%s%lld.%03lld", milli < 0 ? "-" : "+", llabs(milli) / 1000, llabs(milli) % 1000);
}

// The query with the shortest round trip of `count`, added to the peer's samples
static void burst(Peer& peer, int count, int64_t start, int round)
{
    Sample best = {};
    bool any = false;
    for (int i = 0; i < count; ++i)
    {
        Sample sample;
        if (queryOnce(peer, start, sample) && (!any || sample.rtt < best.rtt))
        {
            best = sample;
            any = true;
        }
    }
    best.round = round;
    if (any)
        peer.samples.push_back(best);
}

// The OS's own correction of the system clock right now
static std::string osCorrection()
{
    char text[256] = "";
#ifdef __linux__
    timex tx = {};
    const int state = adjtimex(&tx);
    snprintf(text, sizeof(text),
             "adjtimex state %d freq %ld (ppm << 16) offset %ld tick %ld status 0x%x maxerror %ld us esterror %ld us "
             "constant %ld", state, tx.freq, tx.offset, tx.tick, tx.status, tx.maxerror, tx.esterror, tx.constant);
#elif defined(_WIN32)
    // Through GetProcAddress: the import library that has it is not kernel32.lib
    using AdjustmentFunc = BOOL(WINAPI*)(PDWORD64, PDWORD64, PBOOL);
    static const auto getAdjustment = reinterpret_cast<AdjustmentFunc>(
        GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "GetSystemTimeAdjustmentPrecise"));
    DWORD64 adjustment = 0, increment = 0;
    BOOL disabled = FALSE;
    if (getAdjustment && getAdjustment(&adjustment, &increment, &disabled))
    {
        snprintf(text, sizeof(text), "GetSystemTimeAdjustmentPrecise adjustment %llu increment %llu disabled %d",
                 static_cast<unsigned long long>(adjustment), static_cast<unsigned long long>(increment), disabled);
    }
#endif
    return text;
}

// One CSV line per address answered in this round, and on stdout the round's median offset change since the start
static void logRound(FILE* csv, const std::vector<Peer>& peers, int round)
{
    std::vector<int64_t> changes[kClocks];
    for (const Peer& peer : peers)
    {
        if (peer.samples.empty() || peer.samples.back().round != round)
            continue;
        const Sample& first = peer.samples.front();
        const Sample& s = peer.samples.back();
        if (csv)
        {
            fprintf(csv, "%d,%lld,%s,%s,%d,%lld", round, static_cast<long long>(s.at / 1000000), peer.server.c_str(),
                    peer.address.c_str(), s.stratum, static_cast<long long>(s.rtt));
            for (int k = 0; k < kClocks; ++k)
                fprintf(csv, ",%lld", static_cast<long long>(s.offset[k]));
            fputc('\n', csv);
        }
        if (s.stratum <= 2 && s.rtt < 50000000 && s.at > first.at)
        {
            for (int k = 0; k < kClocks; ++k)
                changes[k].push_back(ppmMilli(first, s, k));
        }
    }
    printf("round %d: %s; median rate since the start, ppm:", round, osCorrection().c_str());
    for (int k = 0; k < kClocks; ++k)
    {
        if (changes[k].empty())
            continue;
        std::sort(changes[k].begin(), changes[k].end());
        printf(" %s ", kClockNames[k]);
        printPpm(stdout, changes[k][changes[k].size() / 2]);
        putchar(';');
    }
    printf(" (%zu)\n", changes[kBte].size());
    fflush(stdout);
}

// A clock's median rate over the addresses that answered this round, since each one's answer in round `from`
static bool medianRate(const std::vector<Peer>& peers, int round, int from, int k, int64_t& milli)
{
    std::vector<int64_t> rates;
    for (const Peer& peer : peers)
    {
        if (peer.samples.empty() || peer.samples.back().round != round)
            continue;
        const auto first = std::find_if(peer.samples.begin(), peer.samples.end(),
                                        [from](const Sample& s) { return s.round == from; });
        const Sample& s = peer.samples.back();
        if (first != peer.samples.end() && s.stratum <= 2 && s.rtt < 50000000 && s.at > first->at)
            rates.push_back(ppmMilli(*first, s, k));
    }
    if (rates.empty())
        return false;
    std::sort(rates.begin(), rates.end());
    milli = rates[rates.size() / 2];
    return true;
}

// From now on the steered clock makes up bte's rate against NTP since the start
static void steer(const std::vector<Peer>& peers, int round)
{
    int64_t slowMilli = 0;
    if (!medianRate(peers, round, 0, kBte, slowMilli))
        return;
    const int64_t bteNow = ticksToNano(timeTicks());
    gSteering = {bteNow, steeredNs(bteNow), 1 / (1 - static_cast<double>(slowMilli) * 1e-9)};
    printf("round %d steering: bte timeTicks ", round);
    printPpm(stdout, slowMilli);
    printf(" ppm since the start, so the steered clock runs that much faster from here\n");
}

#ifndef _WIN32
// The steered clock against CLOCK_MONOTONIC, which the OS keeps at NTP's rate, from the first steered round
struct Apart
{
    int64_t at; // steady clock, ns since the start
    int64_t ns; // the steered clock less CLOCK_MONOTONIC
};

static Clocks gApartBase;
static std::vector<Apart> gApart;
#endif

static void compareSteered(const std::vector<Peer>& peers, int round, int firstSteered, [[maybe_unused]] int64_t start)
{
    printf("round %d steered:", round);
    int64_t milli = 0;
    if (round > firstSteered && medianRate(peers, round, firstSteered, kSteered, milli))
    {
        printf(" rate since round %d ", firstSteered);
        printPpm(stdout, milli);
        printf(" ppm;");
    }
#ifndef _WIN32
    const Clocks c = readClocks();
    if (gApart.empty())
        gApartBase = c;
    const int64_t ns = (c.ns[kSteered] - gApartBase.ns[kSteered]) - (c.ns[kMonotonic] - gApartBase.ns[kMonotonic]);
    gApart.push_back({c.ns[kSteady] - start, ns});
    printf(" ahead of CLOCK_MONOTONIC by %lld us since round %d", static_cast<long long>(ns / 1000), firstSteered);
    if (gApart.size() > 10)
    {
        const Apart& before = gApart[gApart.size() - 11];
        printf(", faster by ");
        printPpm(stdout, (ns - before.ns) * 1000000000 / (gApart.back().at - before.at));
        printf(" ppm over the last 10 rounds");
    }
#endif
    printf("\n");
    fflush(stdout);
}

static std::vector<Peer> resolvePeers()
{
    std::vector<Peer> peers;
    for (const char* server : kServers)
    {
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(server, "123", &hints, &result) != 0)
        {
            printf("%s: no address\n", server);
            continue;
        }
        int added = 0;
        for (addrinfo* rp = result; rp != nullptr && added < 2; rp = rp->ai_next, ++added)
        {
            Peer peer;
            peer.server = server;
            memcpy(&peer.addr, rp->ai_addr, sizeof(peer.addr));
            char text[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &peer.addr.sin_addr, text, sizeof(text));
            peer.address = text;
            peers.push_back(peer);
        }
        freeaddrinfo(result);
    }
    return peers;
}

#ifdef __linux__
static long long kernelFreqMilliPpm()
{
    timex tx = {};
    if (adjtimex(&tx) == -1)
        return 0;
    return (static_cast<long long>(tx.freq) * 1000 >> 16) + (tx.tick - 10000) * 100000LL;
}
#endif

static void report(FILE* f, const std::vector<Peer>& peers, bool markdown)
{
    fprintf(f, markdown ? "| server | address | stratum | samples | round trip ms |" : "server address stratum samples rtt_ms");
    for (const char* name : kClockNames)
        fprintf(f, markdown ? " %s ppm |" : " [%s]", name);
    fputc('\n', f);
    for (int column = 0; markdown && column < 5 + kClocks; ++column)
        fputs(column + 1 < 5 + kClocks ? "|---" : "|---|\n", f);
    std::vector<int64_t> rates[kClocks];
    for (const Peer& peer : peers)
    {
        if (peer.samples.size() < 2)
        {
            fprintf(f, markdown ? "| %s | %s | | %zu |" : "%s %s - %zu", peer.server.c_str(), peer.address.c_str(),
                    peer.samples.size());
            for (int column = 0; markdown && column <= kClocks; ++column)
                fputs(" |", f);
            fputc('\n', f);
            continue;
        }
        const Sample& first = peer.samples.front();
        const Sample& last = peer.samples.back();
        const int64_t rtt = std::max(first.rtt, last.rtt);
        fprintf(f, markdown ? "| %s | %s | %d | %zu | %lld.%03lld |" : "%s %s %d %zu %lld.%03lld", peer.server.c_str(),
                peer.address.c_str(), last.stratum, peer.samples.size(), static_cast<long long>(rtt / 1000000),
                static_cast<long long>(rtt / 1000 % 1000));
        for (int k = 0; k < kClocks; ++k)
        {
            const int64_t milli = ppmMilli(first, last, k);
            fputc(' ', f);
            printPpm(f, milli);
            if (markdown)
                fputs(" |", f);
            if (last.stratum <= 2 && rtt < 50000000)
                rates[k].push_back(milli);
        }
        fputc('\n', f);
    }
    fprintf(f, markdown ? "\nMedian over stratum 1-2 servers under 50 ms:" : "median over stratum 1-2 servers under 50 ms:");
    for (int k = 0; k < kClocks; ++k)
    {
        if (rates[k].empty())
            continue;
        std::sort(rates[k].begin(), rates[k].end());
        fprintf(f, " %s ", kClockNames[k]);
        printPpm(f, rates[k][rates[k].size() / 2]);
        fprintf(f, " ppm (%zu);", rates[k].size());
    }
    fprintf(f, "\n");
}

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? atoi(argv[1]) : 900;
    const int firstSteered = argc > 2 ? atoi(argv[2]) : 10;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::vector<Peer> peers = resolvePeers();
#ifdef __linux__
    const long long freqStart = kernelFreqMilliPpm();
#endif
    FILE* csv = fopen("drift.csv", "w");
    if (csv)
    {
        fprintf(csv, "round,seconds,server,address,stratum,rtt_ns");
        for (const char* name : kClockNames)
            fprintf(csv, ",%s offset ns", name);
        fputc('\n', csv);
    }
    const int64_t start = readClocks().ns[kSteady];
    for (Peer& peer : peers)
        burst(peer, 8, start, 0);
    logRound(csv, peers, 0);
    int round = 1;
    for (; round * 60 < seconds; ++round)
    {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start)) +
                                      std::chrono::seconds(round * 60));
        for (Peer& peer : peers)
            burst(peer, 3, start, round);
        logRound(csv, peers, round);
        if (round >= firstSteered)
        {
            steer(peers, round);
            compareSteered(peers, round, firstSteered, start);
        }
    }
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start)) +
                                  std::chrono::seconds(seconds));
    for (Peer& peer : peers)
        burst(peer, 8, start, round);
    logRound(csv, peers, round);
    if (round > firstSteered)
        compareSteered(peers, round, firstSteered, start);
    if (csv)
        fclose(csv);

    report(stdout, peers, false);
#ifdef __linux__
    const long long freqEnd = kernelFreqMilliPpm();
    printf("kernel NTP frequency correction: start ");
    printPpm(stdout, freqStart);
    printf(" ppm, end ");
    printPpm(stdout, freqEnd);
    printf(" ppm\n");
#endif
    if (const char* summary = getenv("GITHUB_STEP_SUMMARY"))
    {
        if (FILE* f = fopen(summary, "a"))
        {
            fprintf(f, "### Clock rates against NTP over %d s\n\n", seconds);
            report(f, peers, true);
#ifdef __linux__
            fprintf(f, "\nKernel NTP frequency correction (adjtimex): start ");
            printPpm(f, freqStart);
            fprintf(f, " ppm, end ");
            printPpm(f, freqEnd);
            fprintf(f, " ppm\n");
#endif
            fclose(f);
        }
    }
    return 0;
}
