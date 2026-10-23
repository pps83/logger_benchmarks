#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/MathUtilities.h"
#include "quill/core/ThreadContextManager.h"
#include "quill/sinks/Sink.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Slow on its first write, so a burst fills the frontend queue and makes it grow
 */
class SlowStartSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
                 std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view,
                 std::string_view) override
  {
    if (!started.exchange(true))
    {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }

  void flush_sink() override {}

  std::atomic<bool> started{false};
};

/***/
size_t queue_capacity()
{
  detail::ThreadContext* thread_context = detail::FrontendThreadState::thread_context;
  return thread_context->get_spsc_queue_union().unbounded_spsc_queue.producer_capacity();
}

/***/
void log_burst(Logger* logger, size_t* capacity)
{
  for (size_t i = 0; i < 100000; ++i)
  {
    LOG_INFO(logger, "burst message number {}", i);
  }
  *capacity = queue_capacity();
}

/***/
void log_once(Logger* logger, size_t* capacity)
{
  LOG_INFO(logger, "one message");
  *capacity = queue_capacity();
}

/***/
TEST_CASE("grown_queue_is_freed_instead_of_pooled")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  auto sink = Frontend::create_or_get_sink<SlowStartSink>("sink");
  Logger* logger = Frontend::create_or_get_logger("logger", std::move(sink));
  size_t const initial_capacity = detail::next_power_of_two(FrontendOptions::initial_queue_capacity);

  size_t grown_capacity{0};
  std::thread{log_burst, logger, &grown_capacity}.join();
  REQUIRE_GT(grown_capacity, initial_capacity);

  // This thread takes no context of its own, so the spare stays in the pool for the next thread
  REQUIRE(Backend::flush_all(std::chrono::seconds{10}));
  while (detail::ThreadContextManager::instance().has_invalid_thread_context())
  {
    std::this_thread::yield();
  }

  size_t new_thread_capacity{0};
  std::thread{log_once, logger, &new_thread_capacity}.join();
  REQUIRE_EQ(new_thread_capacity, initial_capacity);

  Frontend::remove_logger(logger);
  Backend::stop();
}
