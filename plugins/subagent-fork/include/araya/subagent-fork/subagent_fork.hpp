#pragma once

#include "araya/plugin.hpp"

// The fork subagent provider: a child agent seeded with the parent's
// completed-turn prefix, so it inherits the parent's conversation context
// instead of starting fresh. A feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-subagent-fork-in-process` backend, trimmed to the v1
// seam: the provider owns only lineage (the seeded child session); the
// subagents service owns turn orchestration.
namespace araya::subagent_fork {

// The plugin descriptor: requires `sessions` and `subagents`; registers a
// fork provider under config `provider_name` (default `fork`).
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::subagent_fork
