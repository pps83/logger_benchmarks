#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/MathUtilities.h"
#include "quill/core/ThreadContextManager.h"
#include "quill/sinks/Sink.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Counts the live thread's lines and the ended threads' lines apart
 */
class CountingSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
                 std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view log_message,
                 std::string_view) override
  {
    if (log_message.rfind("live", 0) == 0)
    {
      live_lines.fetch_add(1);
    }
    else
    {
      ended_lines.fetch_add(1);
    }
  }

  void flush_sink() override {}

  std::atomic<size_t> live_lines{0};
  std::atomic<size_t> ended_lines{0};
};

/***/
void log_and_end(Logger* logger, size_t count, size_t* capacity)
{
  for (size_t i = 0; i < count; ++i)
  {
    LOG_INFO(logger, "ended thread line {}", i);
  }
  detail::ThreadContext* thread_context = detail::FrontendThreadState::thread_context;
  *capacity = thread_context->get_spsc_queue_union().unbounded_spsc_queue.producer_capacity();
}

/**
 * Logs its lines, then stays alive until released
 */
void log_and_wait(Logger* logger, size_t count, std::atomic<bool>* logged, std::atomic<bool> const* release)
{
  for (size_t i = 0; i < count; ++i)
  {
    LOG_INFO(logger, "live thread line {}", i);
  }
  logged->store(true);
  while (!release->load())
  {
    std::this_thread::yield();
  }
}

/***/
TEST_CASE("ended_thread_contexts_are_reclaimed_while_another_thread_keeps_the_backend_busy")
{
  ManualBackendWorker* backend = Backend::acquire_manual_backend_worker();
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  // Below the soft limit each poll writes one line, so the live thread's lines stay pending while the others go out
  bo.transit_events_soft_limit = bo.transit_events_hard_limit;
  backend->init(bo);

  auto sink = Frontend::create_or_get_sink<CountingSink>("counting_sink");
  auto& counting_sink = static_cast<CountingSink&>(*sink);
  Logger* logger = Frontend::create_or_get_logger("logger", sink);

  // Nothing is read before the polls below, so every other thread grows its queue past the initial capacity
  size_t constexpr thread_count{4};
  size_t constexpr grown_lines{12000};
  size_t constexpr few_lines{10};
  size_t const initial_capacity = detail::next_power_of_two(FrontendOptions::initial_queue_capacity);
  for (size_t i = 0; i < thread_count; ++i)
  {
    size_t capacity{0};
    std::thread{log_and_end, logger, (i % 2 == 0) ? grown_lines : few_lines, &capacity}.join();
    if (i % 2 == 0)
    {
      REQUIRE_GT(capacity, initial_capacity);
    }
  }

  // Logged after the ended threads' lines, so they are written last and the backend has work until then
  size_t constexpr live_lines{100};
  std::atomic<bool> logged{false};
  std::atomic<bool> release{false};
  std::thread live{log_and_wait, logger, live_lines, &logged, &release};
  while (!logged.load())
  {
    std::this_thread::yield();
  }

  auto& thread_context_manager = detail::ThreadContextManager::instance();
  while (thread_context_manager.has_invalid_thread_context() && (counting_sink.live_lines.load() < live_lines))
  {
    backend->poll_one();
  }
  bool const reclaimed = !thread_context_manager.has_invalid_thread_context();
  size_t const live_written = counting_sink.live_lines.load();
  release.store(true);
  live.join();

  REQUIRE(reclaimed);
  REQUIRE_LT(live_written, live_lines);

  backend->shutdown();
  REQUIRE_EQ(counting_sink.ended_lines.load(), thread_count / 2 * (grown_lines + few_lines));
  REQUIRE_EQ(counting_sink.live_lines.load(), live_lines);
  Frontend::remove_logger(logger);
}
