#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/Rdtsc.h"
#include "quill/sinks/Sink.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Keeps the timestamp of each log statement
 */
class TimestampSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t log_timestamp, std::string_view, std::string_view,
                 std::string const&, std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view,
                 std::string_view) override
  {
    timestamps.push_back(log_timestamp);
  }

  void flush_sink() override {}

  std::vector<uint64_t> timestamps;
};

/***/
TEST_CASE("raw_rdtsc_timestamps")
{
  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  bo.raw_rdtsc_timestamps = true;
  Backend::start(bo);

  auto sink = Frontend::create_or_get_sink<TimestampSink>("timestamp_sink");
  Logger* logger = Frontend::create_or_get_logger("logger", sink);

  uint64_t const before = detail::rdtsc();
  LOG_INFO(logger, "one line");
  uint64_t const after = detail::rdtsc();
  logger->flush_log();

  std::vector<uint64_t> const& timestamps = static_cast<TimestampSink*>(sink.get())->timestamps;
  REQUIRE_EQ(timestamps.size(), size_t{1});
  REQUIRE_GE(timestamps[0], before);
  REQUIRE_LE(timestamps[0], after);

  Frontend::remove_logger(logger);
  Backend::stop();
}
