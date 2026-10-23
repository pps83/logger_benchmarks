#pragma once
#include <memory>
#if __has_include("../ext/DebugTools/DebugTools.h")
#include "../ext/DebugTools/DebugTools.h"
#else
#define HAVE_LEAKFINDER 0

namespace DebugTools
{
struct LeakFinderState
{
    size_t leaks_count;
    size_t leaks_bytes;
    size_t tracked_count;
    size_t tracked_bytes;
};
using MemorySnapshot = int64_t;
static constexpr MemorySnapshot kNoSnapshot = 0;
inline void leakfinder_track_allocations(bool enabled) { (void)enabled; }
inline void leakfinder_enable(bool enabled, bool global = false) { (void)enabled; (void)global; }
inline bool leakfinder_is_enabled() { return false; }
inline void leakfinder_turn_off() {}
inline bool leakfinder_is_turn_off() { return true; }
inline void leakfinder_clear() {}
inline void leakfinder_dump(MemorySnapshot from, MemorySnapshot to) { (void)from; (void)to; }
inline LeakFinderState leakfinder_get_state(MemorySnapshot from, MemorySnapshot to) { (void)from; (void)to; return {}; }
inline MemorySnapshot leakfinder_snapshot() { return kNoSnapshot; }
inline std::shared_ptr<void> leakfinder_init_ptr() { return {}; }
} // namespace DebugTools

struct [[maybe_unused]] LeakFinderScopedDisable
{
};
#endif

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_ASAN 1
#endif
#endif
#ifndef HAVE_ASAN
#define HAVE_ASAN 0
#endif

bool runningOnValgrind();

// ASan and valgrind slow code down many times, so tests cut repetitions that only add more of the same under them
inline bool runningInstrumented()
{
    return HAVE_ASAN || runningOnValgrind();
}
