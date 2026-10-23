#include "doctest/doctest.h"

#include "misc/TestUtilities.h"
#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/GlobalState.h"
#include "quill/LogMacros.h"
#include "quill/sinks/FileSink.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace quill;

static constexpr size_t number_of_messages = 1000;

/***/
void log_messages(Logger* logger, std::string const& thread_name)
{
  for (size_t i = 0; i < number_of_messages; ++i)
  {
    LOG_INFO(logger, "{} message {}", thread_name, i);
  }
}

/***/
void log_then_wait(Logger* logger, std::atomic<bool>* logged, std::atomic<bool>* finish)
{
  log_messages(logger, "outliving");
  logged->store(true);
  while (!finish->load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
}

/***/
TEST_CASE("global_state_writes_every_message_and_frees_quill")
{
  static constexpr char const* filename = "global_state.log";

  auto global_state = std::make_unique<GlobalState>();

  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  Backend::start(bo);

  FileSinkConfig file_sink_config;
  file_sink_config.set_open_mode('w');
  auto file_sink =
    Frontend::create_or_get_sink<FileSink>(filename, file_sink_config, FileEventNotifier{});
  Logger* logger = Frontend::create_or_get_logger("logger", std::move(file_sink));

  std::thread exited{log_messages, logger, std::string{"exited"}};
  exited.join();

  std::atomic<bool> logged{false};
  std::atomic<bool> finish{false};
  std::thread outliving{log_then_wait, logger, &logged, &finish};
  while (!logged.load())
  {
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }

  log_messages(logger, "main");

  // No flush before: destroying the global state writes everything still queued
  global_state.reset();
  REQUIRE(detail::BackendManager::instance_ptr().load() == nullptr);
  REQUIRE(detail::LoggerManager::instance_ptr().load() == nullptr);
  REQUIRE(detail::ThreadContextManager::instance_ptr().load() == nullptr);

  // A thread that logged exits after Quill's state is gone
  finish.store(true);
  outliving.join();

  std::vector<std::string> const file_contents = quill::testing::file_contents(filename);
  REQUIRE_EQ(file_contents.size(), 3 * number_of_messages);
  for (std::string const thread_name : {"exited", "outliving", "main"})
  {
    REQUIRE(quill::testing::file_contains(file_contents, thread_name + " message 0"));
    REQUIRE(quill::testing::file_contains(
      file_contents, thread_name + " message " + std::to_string(number_of_messages - 1)));
  }

  testing::remove_file(filename);
}
