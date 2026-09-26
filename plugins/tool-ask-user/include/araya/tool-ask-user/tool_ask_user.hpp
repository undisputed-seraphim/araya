#pragma once

#include "araya/plugin.hpp"

// The model-facing `ask_user_question` tool over the user-questions service.
// A feature-replication of the deepseek-harness `@deepseek-ai/dsh-tool-ask-user`.
namespace araya::tool_ask_user {

araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_ask_user
