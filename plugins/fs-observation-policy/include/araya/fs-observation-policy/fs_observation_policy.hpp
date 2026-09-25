#pragma once

#include "araya/plugin.hpp"

// The filesystem observation policy: an event-only plugin that records
// every authoritative presence/absence observation and decides the
// fs/write-intent and fs/edit-intent guards from that state. It registers
// no service. Without it, the fs tools keep the bare provider's
// unconditional mutation behavior - exactly the deepseek-harness
// `@deepseek-ai/dsh-fs-observation-policy`.
namespace araya::fs_observation_policy {

// The plugin descriptor: no dependencies, no provisions.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::fs_observation_policy
