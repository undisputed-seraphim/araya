#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"
#include "araya/session/session_types.hpp"

#include <optional>
#include <string>
#include <vector>

// The model-facing `todo_write` tool plus the `todos` session projection:
// each call replaces the whole list as a `todo/write` event, the fold is
// last-write-wins, and a later `turn/start` clears the standing plan. A
// feature-replication of the deepseek-harness `@deepseek-ai/dsh-tool-todo`,
// trimmed to the tool and its projection (no UI panel yet). The projection
// is exposed through `todos_service` so a surface can read the live list.
namespace araya::tool_todo {

// One task entry, mirroring the harness wire shape.
struct todo_item {
	std::string content;
	std::string status;
};

// The read handle for the `todos` projection: the current list for an
// entered session, or nullopt before the first write / after a new turn.
class todos_service {
public:
	virtual ~todos_service() = default;
	virtual std::optional<std::vector<todo_item>> state_of(araya::session::session_id const& id) const = 0;
};

inline constexpr araya::service_key<todos_service> todos_key{"todos", 1};

// The plugin descriptor: requires `tools` and `sessions`; provides `todos`.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::tool_todo
