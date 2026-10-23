#include "doctest/doctest.h"

#include "misc/TestUtilities.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <string>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Backend::start returns before the backend thread runs; a stop that comes first must still end it
 */
TEST_CASE("stop_right_after_start")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};

  for (size_t i = 0; i < 20; ++i)
  {
    Backend::start(bo);
    Backend::stop();
    REQUIRE_FALSE(Backend::is_running());
    REQUIRE_EQ(Backend::get_thread_id(), 0u);
  }
}

/***/
TEST_CASE("log_right_after_start")
{
  static constexpr char const* filename = "log_right_after_start.log";

  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  FileSinkConfig file_sink_config;
  file_sink_config.set_open_mode('w');
  auto file_sink =
    Frontend::create_or_get_sink<FileSink>(filename, file_sink_config, FileEventNotifier{});
  Logger* logger = Frontend::create_or_get_logger("logger", std::move(file_sink));

  LOG_INFO(logger, "logged right after start");
  logger->flush_log();

  std::vector<std::string> const file_contents = quill::testing::file_contents(filename);
  REQUIRE(quill::testing::file_contains(file_contents, "logged right after start"));

  Frontend::remove_logger(logger);
  Backend::stop();
  testing::remove_file(filename);
}
