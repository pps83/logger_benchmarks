#include "doctest/doctest.h"

#include "misc/TestUtilities.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

/***/
void log_until_stopped(Logger* logger, std::atomic<bool>* stop)
{
  for (size_t i = 0; !stop->load(); ++i)
  {
    LOG_INFO(logger, "busy message {}", i);
  }
}

/***/
void wait_while_held(std::atomic<bool> const* hold)
{
  while (hold->load())
  {
    std::this_thread::yield();
  }
}

/**
 * A request taken before the backend decoded a TSC record, and so before its clock exists, ends
 * while a thread keeps logging
 */
TEST_CASE("flush_all_ends_while_a_thread_keeps_logging")
{
  static constexpr char const* filename = "flush_all_while_logging.log";

  // The first poll waits, so the request is taken before any record is decoded
  std::atomic<bool> hold{true};
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  bo.backend_worker_on_poll_begin = [&hold]() { wait_while_held(&hold); };
  Backend::start(bo);

  FileSinkConfig file_sink_config;
  file_sink_config.set_open_mode('w');
  auto file_sink =
    Frontend::create_or_get_sink<FileSink>(filename, file_sink_config, FileEventNotifier{});
  Logger* logger = Frontend::create_or_get_logger("flush_all_while_logging", std::move(file_sink));

  LOG_INFO(logger, "before the request");
  std::atomic<bool> stop{false};
  std::thread producer{log_until_stopped, logger, &stop};

  uint64_t const request = Backend::request_flush_all();
  hold.store(false);

  bool served{false};
  for (int i = 0; (request != 0) && (i < 1000) && !served; ++i)
  {
    served = Backend::wait_flush_all(request, std::chrono::milliseconds{10});
  }
  stop.store(true);
  producer.join();
  std::vector<std::string> const file_contents = quill::testing::file_contents(filename);

  // The backend's poll callback reads hold, so the backend stops before an assertion can leave the scope
  Frontend::remove_logger(logger);
  Backend::stop();
  testing::remove_file(filename);

  REQUIRE_NE(request, 0u);
  REQUIRE(served);
  REQUIRE(quill::testing::file_contains(file_contents, "before the request"));
}
