#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
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
 * Holds the backend inside each write until the test releases it
 */
class HoldingSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
                 std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view,
                 std::string_view) override
  {
    size_t const write = writes.fetch_add(1) + 1;
    while (released.load() < write)
    {
      std::this_thread::yield();
    }
  }

  void flush_sink() override {}

  std::atomic<size_t> writes{0};
  std::atomic<size_t> released{0};
};

/***/
void wait_for_writes(HoldingSink const& sink, size_t count)
{
  while (sink.writes.load() < count)
  {
    std::this_thread::yield();
  }
}

/***/
TEST_CASE("spare_thread_context_is_made_while_the_backend_is_busy")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  auto sink = Frontend::create_or_get_sink<HoldingSink>("holding_sink");
  auto& holding_sink = static_cast<HoldingSink&>(*sink);
  Logger* logger = Frontend::create_or_get_logger("logger", sink);

  // The first log takes this thread's context and asks the backend for a spare
  LOG_INFO(logger, "first");
  wait_for_writes(holding_sink, 1);

  // The second message is queued before the first write ends, so the backend never goes idle
  LOG_INFO(logger, "second");
  holding_sink.released.store(1);
  wait_for_writes(holding_sink, 2);

  REQUIRE_EQ(detail::ThreadContextManager::instance().pooled_thread_context_count(), 1u);

  holding_sink.released.store(2);
  logger->flush_log();
  Frontend::remove_logger(logger);
  Backend::stop();
}
