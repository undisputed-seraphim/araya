#pragma once

#include "araya/plugin.hpp"

// The same-session automatic goal continuation driver: while a session's goal
// is active and armed, it queues one goal-round prompt per idle boundary and
// fences the reservation at the agent pre-step. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-goal-round-driver`, trimmed to the round
// reservation and its pre-step fence (no durability checkpoint, no cancelled
// attempt bookkeeping, no deferred-context wrap-up).
namespace araya::goal_round_driver {

// The plugin descriptor: requires `agents`/`goals`/`sessions`; provides
// nothing (it is a pure lifecycle driver).
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::goal_round_driver
