#pragma once

#include "quill/core/FrontendOptions.h"

struct CustomFrontendOptions : quill::FrontendOptions
{
#ifdef QUILL_USE_BOUNDED_DROPPING_QUEUE
  static constexpr quill::QueueType queue_type = quill::QueueType::BoundedDropping;
  static constexpr size_t initial_queue_capacity = 262'144;
#else
  static constexpr quill::QueueType queue_type = quill::QueueType::UnboundedBlocking;
  #ifdef BENCH_QUILL_QUEUE_256K
  static constexpr size_t initial_queue_capacity = 262'144; // as bte's
  #elif defined(BENCH_QUILL_QUEUE_512K)
  static constexpr size_t initial_queue_capacity = 524'288;
  #else
  static constexpr size_t initial_queue_capacity = 131'072;
  #endif
#endif

#ifdef QUILL_USE_HUGE_PAGES
  static constexpr quill::HugePagesPolicy huge_pages_policy = quill::HugePagesPolicy::Always;
#endif
};
