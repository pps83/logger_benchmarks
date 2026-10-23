#include "doctest/doctest.h"

#include "quill/Backend.h"
#include "quill/Frontend.h"
#include "quill/LogMacros.h"
#include "quill/core/Rdtsc.h"
#include "quill/sinks/Sink.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace quill;

/**
 * Keeps each message
 */
class MessageSink : public Sink
{
public:
  void write_log(MacroMetadata const*, uint64_t, std::string_view, std::string_view, std::string const&,
                 std::string_view, LogLevel, std::string_view, std::string_view,
                 std::vector<std::pair<std::string, std::string>> const*, std::string_view log_message,
                 std::string_view) override
  {
    messages.emplace_back(log_message);
  }

  void flush_sink() override {}

  std::vector<std::string> messages;
};

static constexpr MacroMetadata compact_metadata{
  "", "", "{}", nullptr, LogLevel::Info, MacroMetadata::Event::Log};

// The record's tagged word, then one uint32_t
static void decode_compact(std::byte*& data, DynamicFormatArgStore& args_store)
{
  uint32_t value;
  std::memcpy(&value, data + sizeof(uint64_t), sizeof(value));
  data += sizeof(uint64_t) + sizeof(value);
  args_store.push_back(value);
}

/***/
TEST_CASE("compact_records")
{
  auto sink = Frontend::create_or_get_sink<MessageSink>("message_sink");
  Logger* logger = Frontend::create_or_get_logger("logger", sink, PatternFormatterOptions{"%(message)"});

  BackendOptions bo;
  bo.error_notifier = [](std::string const&) {};
  bo.compact_decoder = decode_compact;
  bo.compact_metadata = &compact_metadata;
  bo.compact_logger = logger;
  Backend::start(bo);

  // The thread's first log sets up its queue
  LOG_INFO(logger, "regular {}", 1);

  detail::QueueProducer<size_t>& producer =
    detail::FrontendThreadState::thread_context->get_spsc_queue<FrontendOptions::queue_type>().producer();

  for (uint32_t i = 0; i < 3; ++i)
  {
    size_t constexpr size = 2 * sizeof(uint64_t) + sizeof(uint32_t);
    REQUIRE_GE(producer.reader_pos_cache_plus_capacity - producer.writer_pos, size);

    std::byte* record = producer.storage + (producer.writer_pos & producer.mask);
    uint64_t const timestamp = detail::rdtsc();
    uint64_t const tag = 1;
    std::memcpy(record, &timestamp, sizeof(timestamp));
    std::memcpy(record + sizeof(uint64_t), &tag, sizeof(tag));
    std::memcpy(record + 2 * sizeof(uint64_t), &i, sizeof(i));
    producer.writer_pos += size;
    producer.atomic_writer_pos.store(producer.writer_pos, std::memory_order_release);
  }

  LOG_INFO(logger, "regular {}", 2);
  logger->flush_log();

  std::vector<std::string> const expected{"regular 1", "0", "1", "2", "regular 2"};
  REQUIRE_EQ(static_cast<MessageSink*>(sink.get())->messages, expected);

  Frontend::remove_logger(logger);
  Backend::stop();
}
