/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/core/Common.h"

#ifndef QUILL_MODULE
  #include <atomic>
  #include <cstddef>
#endif

QUILL_BEGIN_NAMESPACE

namespace detail
{

#if defined(_WIN32) && defined(_MSC_VER) && !defined(__GNUC__)
  #pragma warning(push)
  #pragma warning(disable : 4324)
#endif

/**
 * The producer's side of a BoundedSPSCQueueImpl, light enough to write records without the queue.
 *
 * n bytes fit when reader_pos_cache_plus_capacity - writer_pos >= n. They go to
 * storage + (writer_pos & mask) and may run past the end of the ring, and become visible to the
 * consumer when the advanced writer_pos is stored in atomic_writer_pos.
 */
template <typename T>
struct QueueProducer
{
  std::byte* storage{nullptr};
  T mask{0};
  T writer_pos{0};
  T reader_pos_cache_plus_capacity{0};

  alignas(QUILL_CACHE_LINE_ALIGNED) std::atomic<T> atomic_writer_pos{0};
};

#if defined(_WIN32) && defined(_MSC_VER) && !defined(__GNUC__)
  #pragma warning(pop)
#endif

} // namespace detail

QUILL_END_NAMESPACE
