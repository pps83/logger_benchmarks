#include "DestructionManager.h"
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>
#include <assert.h>
#include <stdlib.h>

struct DestructionManagerImpl
{
    struct Entry
    {
        int index;
        std::shared_ptr<void> ptr;
    };
    std::mutex mx;
    std::vector<Entry> entries;
    bool destroying = false;

    void add(int index, std::shared_ptr<void> ptr)
    {
        assert(ptr);
        std::scoped_lock lock(mx);
        // A singleton that first appears in the teardown can be given no place in its order: a bug in the caller
        if (destroying)
            abort();
        if (entries.empty())
            entries.reserve(32);
        entries.push_back({index, std::move(ptr)});
    }

    void destroyAll()
    {
        {
            std::scoped_lock lock(mx);
            destroying = true;
        }
        // Sort ascending: lower index = destroyed later; stable so equal-index entries preserve
        // registration order here, and are then destroyed in reverse (LIFO) by the loops below.
        std::stable_sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.index < b.index; });
        // Copy the first N entries (lowest indices, destroyed last) to stack_entries so they execute
        // after `entries` is freed. This ensures the dynamic allocation from DestructionManagerImpl::entries
        // is gone by the time the lowest-index objects (e.g. mem leak tracking at index 2) run.
        std::array<Entry, 8> stack_entries;
        size_t stack_count = std::min(entries.size(), stack_entries.size());
        for (size_t i = 0; i < stack_count; ++i)
            stack_entries[i] = std::move(entries[i]);
        // Destroy remaining higher-index entries first, in descending order.
        for (size_t i = entries.size(); i > stack_count; --i)
            entries[i-1].ptr.reset();
        entries.clear();
        entries.shrink_to_fit(); // release dynamic entries
        // Destroy stack entries last, descending (reverse of ascending storage = LIFO for equal index).
        for (size_t i = stack_count; i > 0; --i)
            stack_entries[i-1].ptr.reset();
    }

    ~DestructionManagerImpl()
    {
        destroyAll();
    }
};

// Created on first use, so registrations from earlier initializers are not lost
static DestructionManagerImpl& destructionManager()
{
    static DestructionManagerImpl instance;
    return instance;
}

// Created before user-level statics, so it is destroyed after them
struct DestructionManagerEarlyInit
{
    DestructionManagerEarlyInit()
    {
        destructionManager();
    }
};

#ifdef __GNUC__
static DestructionManagerEarlyInit gDestructionManagerEarlyInit __attribute__((init_priority(101)));
#else
#ifdef _MSC_VER
#pragma warning(disable : 4073) // initializers put in lib initialization area
#pragma init_seg(lib)
#endif
static DestructionManagerEarlyInit gDestructionManagerEarlyInit;
#endif

void DestructionManager::registerShared(int index, std::shared_ptr<void> ptr)
{
    destructionManager().add(index, std::move(ptr));
}

void DestructionManager::destroyNow()
{
    destructionManager().destroyAll();
}
