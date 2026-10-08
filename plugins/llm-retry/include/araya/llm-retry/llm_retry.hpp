#pragma once

#include "araya/plugin.hpp"

// The model-request retry executor: a listener on the agent loop's
// `agent/request-error` waterfall that re-runs a failed step at its durable
// boundary according to the route's provider-owned retry policy. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-llm-retry`,
// trimmed to transient recovery (no durable retry events).
namespace araya::llm_retry {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::llm_retry
