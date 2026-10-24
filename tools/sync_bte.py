#!/usr/bin/env python3
"""Copy backtest-engine's logger, its tests and log_stress into bte/, unchanged, from a backtest-engine checkout."""

from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

# Every file of the logger and its tests; nothing in them includes a backtest-engine file not listed here
FILES = [
    "src/AsyncLog.cpp",
    "src/AsyncLog.h",
    "src/DestructionManager.cpp",
    "src/DestructionManager.h",
    "src/GetProcAddr.cpp",
    "src/GetProcAddr.h",
    "src/SyncLog.cpp",
    "src/SyncLog.h",
    "src/UninitializedMemoryHacks.h",
    "src/date.h",
    "src/debug.h",
    "src/format.cpp",
    "src/format.h",
    "src/libdivide.h",
    "src/log.cpp",
    "src/log.h",
    "src/random.cpp",
    "src/random.h",
    "src/sysutil.cpp",
    "src/sysutil.h",
    "src/time.cpp",
    "src/time_tsc.cpp",
    "src/test/LogCallSiteCheck.cmake",
    "src/test/LogCallSites.cpp",
    "src/test/LogCrashTest.cpp",
    "src/test/LogEarlyInit.cpp",
    "src/test/LogStress.cpp",
    "src/test/LogTest.cpp",
    "src/test/TestFile.h",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("bte", type=Path, help="the backtest-engine checkout to copy from")
    args = parser.parse_args()

    source = args.bte.resolve()
    destination = Path(__file__).resolve().parents[1] / "bte"
    missing = [name for name in FILES if not (source / name).is_file()]
    if missing:
        print("not in " + str(source) + ": " + ", ".join(missing), file=sys.stderr)
        return 1
    for name in FILES:
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source / name, target)
    print(f"copied {len(FILES)} files from {source} to {destination}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
