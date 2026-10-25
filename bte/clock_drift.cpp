// clock_drift [seconds]: each clock's rate against each NTP server address on its own, in ppm (positive: slow)
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

enum Clock { kBte, kTsc, kSystem, kSteady, kClocks };
static const char* const kClockNames[kClocks] = {"bte timeTicks", "bte tsc", "system", "steady"};

struct Clocks
{
    int64_t ns[kClocks];
};

static Clocks readClocks()
{
    Clocks c;
    c.ns[kBte] = ticksToNano(timeTicks());
    c.ns[kTsc] = tsc::ticksToNano(tsc::timeTicks());
    c.ns[kSystem] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    c.ns[kSteady] = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    return c;
}

struct Sample
{
    int64_t at;     // steady clock, ns since the start
    int64_t rtt;    // ns, the server's own time taken off
    int64_t offset[kClocks];
    int stratum;
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

// The query with the shortest round trip of `count`, added to the peer's samples
static void burst(Peer& peer, int count, int64_t start)
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
    if (any)
        peer.samples.push_back(best);
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

// ppm with 3 decimals as an integer: the offset's change in ns over the elapsed ns, times 1e9
static int64_t ppmMilli(const Sample& first, const Sample& last, int k)
{
    return (last.offset[k] - first.offset[k]) * 1000000000 / (last.at - first.at);
}

static void printPpm(FILE* f, int64_t milli)
{
    fprintf(f, "%s%lld.%03lld", milli < 0 ? "-" : "+", llabs(milli) / 1000, llabs(milli) % 1000);
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
    fprintf(f, markdown ? "\n|---|---|---|---|---|---|---|---|---|\n" : "\n");
    std::vector<int64_t> rates[kClocks];
    for (const Peer& peer : peers)
    {
        if (peer.samples.size() < 2)
        {
            fprintf(f, markdown ? "| %s | %s | | %zu | | | | | |\n" : "%s %s - %zu\n", peer.server.c_str(),
                    peer.address.c_str(), peer.samples.size());
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
    const int seconds = argc > 1 ? atoi(argv[1]) : 600;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    std::vector<Peer> peers = resolvePeers();
#ifdef __linux__
    const long long freqStart = kernelFreqMilliPpm();
#endif
    const int64_t start = readClocks().ns[kSteady];
    for (Peer& peer : peers)
        burst(peer, 8, start);
    for (int minute = 1; minute * 60 < seconds; ++minute)
    {
        std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start)) +
                                      std::chrono::seconds(minute * 60));
        for (Peer& peer : peers)
            burst(peer, 3, start);
        printf("minute %d done\n", minute);
        fflush(stdout);
    }
    std::this_thread::sleep_until(std::chrono::steady_clock::time_point(std::chrono::nanoseconds(start)) +
                                  std::chrono::seconds(seconds));
    for (Peer& peer : peers)
        burst(peer, 8, start);

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
