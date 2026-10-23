#pragma once
#include <memory>

// Controls destruction order of global singletons at process exit.
//
// Objects created with DestructionManager::make_shared are destroyed in descending index order.
// Higher index = destroyed first. Lower index = destroyed later (i.e. the object outlives
// higher-index dependents).
//
// Example:
//   // HttpPool (0) must outlive HttpClient (10), so HttpClient is destroyed first.
//   auto httpClient = DestructionManager::make_shared<HttpClient>(10, ...);
//   auto httpPool   = DestructionManager::make_shared<HttpPool>(0, ...);
//
// Thread-safe for concurrent registrations.

class DestructionManager
{
public:
    template<typename T>
    static void manage(int index, std::shared_ptr<T> ptr)
    {
        registerShared(index, std::move(ptr));
    }

    template<typename T, typename... Args>
    static std::shared_ptr<T> make_shared(int index, Args&&... args)
    {
        auto ptr = std::make_shared<T>(std::forward<Args>(args)...);
        registerShared(index, ptr);
        return ptr;
    }

    // Runs the ordered teardown now (idempotent; main thread, after all singleton use); later LOG and LLOG are dropped
    static void destroyNow();

private:
    static void registerShared(int index, std::shared_ptr<void> ptr);
};
