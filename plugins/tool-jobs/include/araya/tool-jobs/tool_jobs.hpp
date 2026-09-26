#pragma once

#include "araya/plugin.hpp"

// The model-facing job controls over the `jobs` registry: `job_output`,
// `job_list`, and `job_kill`, plus completion-notice delivery. Loading the
// plugin attaches the controller producers need. A feature-replication of the
// deepseek-harness `@deepseek-ai/dsh-tool-jobs`, trimmed to the in-process
// shape.
//
// Divergence from the harness (recorded): a settled job's notice is appended to
// the owning session as a durable user message, so an idle owner sees it at its
// next step or turn - there is no agent inbox to inject it into a running step
// or to wake an idle owner.
namespace araya::tool_jobs {

// The plugin descriptor: requires `jobs`, `sessions`, `system-prompt`, and
// `tools`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_jobs
