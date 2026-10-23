/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/Backend.h"
#include "quill/backend/BackendManager.h"
#include "quill/backend/SignalHandler.h"
#include "quill/core/Attributes.h"
#include "quill/core/LoggerManager.h"
#include "quill/core/MetricManager.h"
#include "quill/core/SinkManager.h"
#include "quill/core/ThreadContextManager.h"

QUILL_BEGIN_NAMESPACE

QUILL_BEGIN_EXPORT

/**
 * Owns Quill's process-wide state for an application that orders its own teardown.
 *
 * Without a GlobalState that state is created on first use and never destroyed, and Backend::start
 * registers an atexit handler that stops the backend. Create one, and only one, before
 * Backend::start: no atexit handler is registered, and destroying it stops the backend, writing
 * every queued message, then frees the loggers, sinks and thread contexts. Nothing may use Quill
 * once it is destroyed.
 */
class GlobalState
{
public:
  GlobalState()
  {
    (void)detail::ThreadContextManager::instance();
    (void)detail::SinkManager::instance();
    (void)detail::LoggerManager::instance();
    (void)detail::MetricManager::instance();
    (void)detail::SignalHandlerContext::instance();
    detail::BackendManager::instance().set_atexit_registered();
  }

  GlobalState(GlobalState const&) = delete;
  GlobalState& operator=(GlobalState const&) = delete;

  ~GlobalState()
  {
    Backend::stop();
    detail::BackendManager::destroy_instance();
    detail::LoggerManager::destroy_instance();
    detail::SinkManager::destroy_instance();
    detail::ThreadContextManager::destroy_instance();
    detail::MetricManager::destroy_instance();
    detail::SignalHandlerContext::destroy_instance();
  }
};

QUILL_END_EXPORT

QUILL_END_NAMESPACE
