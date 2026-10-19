"""Runs the first-LOG benchmarks, one process per measurement, and prints the results per logger and thread count.

py run_first_log.py <bin dir> <out dir> [--runs 50] [--threads 1,2,4,8,16] [--loggers bte,quill_fork,quill,fmtlog]
    Each thread's first LOG in us (all threads' p50/p90/max, the median of each run's slowest) and the late thread's.
py run_first_log.py <bin dir> <out dir> --end exit|abort|crash [--delays 0:2000:20] [--repeat 1] [--lines 0]
    Ends each process that many us after the signal, while the threads are in their first LOG or, with --lines, in
    the lines each logs after it; counts exit codes and hangs, and runs whose log skips or repeats a thread's line.
Each invocation runs in a new folder under <out dir>, a folder per logger, where the loggers write their files.
"""

import argparse
import collections
import datetime
import os
import re
import statistics
import subprocess
import sys
import time


def percentile(values, fraction):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, max(0, int(len(ordered) * fraction + 0.5) - 1))]


def parse_line(stderr):
    for line in stderr.splitlines():
        if line.startswith("first_log "):
            words = line.split()
            wake_at = words.index("wake_ns")
            log_at = words.index("log_ns")
            late_at = words.index("late_ns")
            return ([int(w) for w in words[wake_at + 1:log_at]], [int(w) for w in words[log_at + 1:late_at]],
                    int(words[late_at + 1]))
    return None


def run(exe, args, cwd, timeout):
    try:
        done = subprocess.run([exe] + args, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                              encoding="utf-8", errors="replace", timeout=timeout)
        return done.returncode, done.stderr
    except subprocess.TimeoutExpired:
        return None, ""


def code_name(code):
    if code is None:
        return "hang"
    if code < 0:
        code += 1 << 32
    return str(code) if code < 256 else hex(code)


def measure(exe, cwd, threads, runs, timeout):
    firsts = []
    slowest = []
    lates = []
    failures = collections.Counter()
    for _ in range(runs):
        code, stderr = run(exe, [str(threads)], cwd, timeout)
        parsed = parse_line(stderr)
        if code != 0 or parsed is None:
            failures[code_name(code)] += 1
            continue
        _, logs, late = parsed
        firsts += logs
        slowest.append(max(logs))
        lates.append(late)
    if not firsts:
        return "no results, " + ", ".join(f"{name} x{count}" for name, count in failures.items())
    us = lambda ns: f"{ns / 1000:.1f}"
    text = (f"first LOG us p50 {us(percentile(firsts, 0.5))} p90 {us(percentile(firsts, 0.9))} max {us(max(firsts))}, "
            f"slowest per run p50 {us(statistics.median(slowest))}, "
            f"late p50 {us(percentile(lates, 0.5))} max {us(max(lates))}")
    if failures:
        text += ", failed: " + ", ".join(f"{name} x{count}" for name, count in failures.items())
    return text


LOG_FILES = {"bte": "log.txt", "quill": "first_log_quill.log", "quill_fork": "first_log_quill.log",
             "fmtlog": "first_log_fmtlog.log"}
LOGGED_LINE = re.compile(r"Thread (\d+) line (\d+)")


def gaps_in_log(path):
    """Lines each thread's sequence skips or repeats, and the lines written"""
    try:
        with open(path, encoding="utf-8", errors="replace") as log:
            text = log.read()
    except FileNotFoundError:
        return 0, 0
    next_line = {}
    gaps = 0
    for match in LOGGED_LINE.finditer(text):
        thread, line = int(match[1]), int(match[2])
        if line != next_line.get(thread, 0):
            gaps += 1
        next_line[thread] = line + 1
    return gaps, sum(next_line.values())


def end_runs(exe, cwd, log_file, threads, end, delays, repeat, lines, timeout):
    codes = collections.Counter()
    delays_of = collections.defaultdict(list)
    gap_delays = []
    written = 0
    for delay in delays:
        for _ in range(repeat):
            code, _ = run(exe, [str(threads), end, str(delay), str(lines)], cwd, timeout)
            codes[code_name(code)] += 1
            delays_of[code_name(code)].append(delay)
            gaps, count = gaps_in_log(os.path.join(cwd, log_file))
            written += count
            if gaps:
                gap_delays.append(delay)
    text = ", ".join(f"{name} x{count}" for name, count in sorted(codes.items()))
    usual = codes.most_common(1)[0][0]
    for name, at in sorted(delays_of.items()):
        if name != usual:
            text += f", {name} at us {at}"
    text += f", {written} lines written, runs with gaps: {len(gap_delays)}"
    if gap_delays:
        text += f" at us {gap_delays}"
    return text


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("bin_dir")
    parser.add_argument("out_dir")
    parser.add_argument("--runs", type=int, default=50)
    parser.add_argument("--threads", default="1,2,4,8,16")
    parser.add_argument("--loggers", default="bte,quill_fork,quill,fmtlog")
    parser.add_argument("--end", choices=["exit", "abort", "crash"])
    parser.add_argument("--delays", default="0:2000:20")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--lines", type=int, default=0)
    parser.add_argument("--timeout", type=float, default=20)
    options = parser.parse_args()

    first, last, step = (int(part) for part in options.delays.split(":"))
    delays = list(range(first, last + 1, step))
    session = os.path.join(options.out_dir, datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    for logger in options.loggers.split(","):
        exe = os.path.join(options.bin_dir, f"benchmark_{logger}_first_log.exe" if os.name == "nt"
                           else f"benchmark_{logger}_first_log")
        cwd = os.path.join(session, logger)
        os.makedirs(cwd)
        for threads in (int(count) for count in options.threads.split(",")):
            began = time.perf_counter()
            if options.end:
                text = end_runs(exe, cwd, LOG_FILES[logger], threads, options.end, delays, options.repeat,
                                options.lines, options.timeout)
            else:
                text = measure(exe, cwd, threads, options.runs, options.timeout)
            print(f"{logger} threads {threads}: {text} ({time.perf_counter() - began:.0f} s)", flush=True)


if __name__ == "__main__":
    sys.exit(main())
