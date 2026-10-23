#include "doctest/doctest.h"

#include "misc/TestUtilities.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

static constexpr size_t number_of_messages = 1000;

/**
 * Constructed before its thread's first log, so destroyed after the thread released its context
 */
struct LogsAtThreadExit
{
  Logger* logger{nullptr};
  bool* released_when_logging{nullptr};

  ~LogsAtThreadExit()
  {
    if (logger)
    {
      *released_when_logging = detail::FrontendThreadState::released;
      LOG_INFO(logger, "logged after release");
    }
  }
};

/***/
void log_then_exit(Logger* logger, bool* released_when_logging)
{
  thread_local LogsAtThreadExit logs_at_exit;
  logs_at_exit.logger = logger;
  logs_at_exit.released_when_logging = released_when_logging;
  LOG_INFO(logger, "logged before exit");
  logger->flush_log();
}

/***/
void log_messages(Logger* logger)
{
  for (size_t i = 0; i < number_of_messages; ++i)
  {
    LOG_INFO(logger, "next thread message {}", i);
  }
}

/***/
TEST_CASE("log_after_thread_context_released")
{
  static constexpr char const* filename = "log_after_thread_context_released.log";

  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  FileSinkConfig file_sink_config;
  file_sink_config.set_open_mode('w');
  auto file_sink =
    Frontend::create_or_get_sink<FileSink>(filename, file_sink_config, FileEventNotifier{});
  Logger* logger = Frontend::create_or_get_logger("logger", std::move(file_sink));

  bool released_when_logging{false};
  std::thread exiting{log_then_exit, logger, &released_when_logging};
  exiting.join();
  REQUIRE(released_when_logging);

  // May take over the released context once the backend pools it
  std::this_thread::sleep_for(std::chrono::milliseconds{50});
  std::thread next{log_messages, logger};
  next.join();

  logger->flush_log();
  Frontend::remove_logger(logger);
  Backend::stop();

  std::vector<std::string> const file_contents = quill::testing::file_contents(filename);
  REQUIRE(quill::testing::file_contains(file_contents, "logged before exit"));
  REQUIRE_FALSE(quill::testing::file_contains(file_contents, "logged after release"));
  REQUIRE_EQ(file_contents.size(), number_of_messages + 1);
  for (size_t i = 0; i < number_of_messages; ++i)
  {
    REQUIRE(quill::testing::file_contains(file_contents, "next thread message " + std::to_string(i)));
  }

  testing::remove_file(filename);
}
