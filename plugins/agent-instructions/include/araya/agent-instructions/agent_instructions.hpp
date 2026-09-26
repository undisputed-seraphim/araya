#pragma once

#include "araya/plugin.hpp"

// Workspace instructions (AGENTS.md-compatible) as a system-prompt section:
// discover the candidate files from the session cwd up to the project root,
// read and render them within a byte budget, and contribute the result at
// the AGENT_INSTRUCTIONS order. A feature-replication of the deepseek-harness
// `@deepseek-ai/dsh-agent-instructions` baseline, trimmed to the read-only
// discovery/render path (no fs-touch reconciliation, no user-global file).
namespace araya::agent_instructions {

// The plugin descriptor: requires `system-prompt`; provides nothing.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::agent_instructions
