#include "first_log_bench.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/Logger.h"
#include "quill/sinks/FileSink.h"

#include <utility>

struct QueueOptions : quill::FrontendOptions
{
  static constexpr size_t initial_queue_capacity = 262'144; // as bte's
};

using Frontend = quill::FrontendImpl<QueueOptions>;
using Logger = quill::LoggerImpl<QueueOptions>;

// The first LOG starts the logger, as bte's does
static Logger* start_quill()
{
  quill::Backend::start(quill::BackendOptions{});
  quill::FileSinkConfig config;
  config.set_open_mode('w');
  config.set_filename_append_option(quill::FilenameAppendOption::None);
  auto sink = Frontend::create_or_get_sink<quill::FileSink>("first_log_quill.log", config, quill::FileEventNotifier{});
  return Frontend::create_or_get_logger("root", std::move(sink));
}

static void log_first(int thread, int line)
{
  static Logger* const logger = start_quill();
  LOG_INFO(logger, "Thread {} line {}", thread, line);
}

int main(int argc, char* argv[])
{
  return run_first_log(argc, argv, log_first);
}
