#include "araya/tool-todo/tool_todo.hpp"

#include "araya/config.hpp"
#include "araya/session/projection.hpp"
#include "araya/session/store.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::tool_todo {
namespace {

using araya::session::event_projection;
using araya::session::projection_state;
using araya::session::session_event;
using araya::session::session_header;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr araya::config_key<bool> allow_parallel_key{"allow_parallel_in_progress"};

constexpr std::string_view status_pending = "pending";
constexpr std::string_view status_in_progress = "in_progress";
constexpr std::string_view status_completed = "completed";

bool valid_status(std::string_view status) {
	return status == status_pending || status == status_in_progress || status == status_completed;
}

using todo_list = std::vector<todo_item>;
// nullopt = no active list (before the first write, or after a new turn).
using todo_state = std::optional<todo_list>;

// The `todos` read handle over the projection tracker the plugin owns.
struct todos_impl : todos_service {
	araya::session::projection_state<todo_state>* tracker = nullptr;

	std::optional<std::vector<todo_item>> state_of(session_id const& id) const override {
		auto const* state = tracker ? tracker->state_of(id) : nullptr;
		if (!state || !*state)
			return std::nullopt;
		return **state;
	}
};

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

std::string trim(std::string_view text) {
	auto const first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto const last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

std::string describe(bool allow_parallel) {
	std::string text =
		"Record and update a structured task list for the current work. Send the ENTIRE list every call — it REPLACES "
		"the previous list (there are no partial updates, no per-item edits). Use it to plan multi-step work and show "
		"progress: add one todo per concrete step before you start. ";
	text +=
		allow_parallel
			? "Mark every todo being actively worked on `in_progress` — several at once when work genuinely runs in "
			  "parallel (e.g. concurrent subagents or background commands), one for sequential work; while work "
			  "remains, at least one task should be `in_progress`. "
			: "Keep AT MOST ONE todo `in_progress` at a time; while work remains, exactly one active task should be "
			  "`in_progress`. ";
	text +=
		"Mark a todo `completed` the moment it is done (do not batch completions), and allow no `in_progress` item "
		"only "
		"once all work is complete. Skip the list for trivial single-step tasks. Statuses: `pending` (not started), "
		"`in_progress` (being worked on now), `completed` (finished).";
	return text;
}

boost::json::value todos_schema() {
	boost::json::object item{
		{"type", "object"},
		{"additionalProperties", false},
		{"properties",
		 boost::json::object{
			 {"content",
			  boost::json::object{
				  {"type", "string"},
				  {"required", true},
				  {"description", "What the task is — a short imperative line."}}},
			 {"status",
			  boost::json::object{
				  {"type", "string"},
				  {"required", true},
				  {"enum", boost::json::array{status_pending, status_in_progress, status_completed}},
				  {"description", "pending (not started) | in_progress (now) | completed (done)."}}}}},
	};
	return boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"todos",
			  boost::json::object{
				  {"type", "array"},
				  {"required", true},
				  {"description", "The COMPLETE task list, replacing any previous list."},
				  {"items", std::move(item)}}}}},
		{"required", boost::json::array{"todos"}},
	};
}

// Validate the model's list and canonicalize it: trimmed non-empty unique
// content, known statuses, and (unless parallel) at most one in_progress.
// An error string is returned on rejection.
std::optional<todo_list> canonicalize(boost::json::value const& arguments, bool allow_parallel, std::string& error) {
	auto const* object = arguments.if_object();
	auto const* raw = object ? araya::util::json::get_array(*object, "todos") : nullptr;
	if (!raw) {
		error = "invalid todos: `todos` must be an array";
		return std::nullopt;
	}
	todo_list todos;
	std::vector<std::string> seen;
	std::size_t active = 0;
	for (auto const& entry : *raw) {
		auto const* item = entry.if_object();
		if (!item) {
			error = "invalid todo: each item must be an object";
			return std::nullopt;
		}
		std::string content = araya::util::json::get_string(*item, "content");
		content = trim(content);
		if (content.empty()) {
			error = "invalid todo: `content` must be a non-empty string";
			return std::nullopt;
		}
		auto const status = araya::util::json::get_string(*item, "status");
		if (!valid_status(status)) {
			error = "invalid todo: `status` must be pending, in_progress, or completed";
			return std::nullopt;
		}
		if (std::find(seen.begin(), seen.end(), content) != seen.end()) {
			error = "invalid todos: duplicate content \"" + content + "\"";
			return std::nullopt;
		}
		seen.push_back(content);
		if (status == status_in_progress)
			++active;
		todos.push_back(todo_item{std::move(content), status});
	}
	if (!allow_parallel && active > 1) {
		error = "invalid todos: at most one task may be in_progress (got " + std::to_string(active) + ")";
		return std::nullopt;
	}
	return todos;
}

boost::json::value to_json(todo_list const& todos) {
	boost::json::array array;
	for (auto const& todo : todos)
		array.push_back(boost::json::object{{"content", todo.content}, {"status", todo.status}});
	return boost::json::value{{"todos", std::move(array)}};
}

araya::task<tool_result>
handle_todo(std::shared_ptr<session_store> store, bool allow_parallel, tool_context const& ctx) {
	std::string error;
	auto todos = canonicalize(ctx.arguments, allow_parallel, error);
	if (!todos)
		co_return error_result(std::move(error));
	if (ctx.session.empty())
		co_return error_result("todo_write requires an owning agent session");

	auto session = store->get(session_id{ctx.session});
	if (!session)
		co_return error_result("todo_write: the calling session is not entered");
	session->append("todo/write", to_json(*todos));

	std::size_t pending = 0;
	std::size_t in_progress = 0;
	std::size_t completed = 0;
	for (auto const& todo : *todos) {
		if (todo.status == status_pending)
			++pending;
		else if (todo.status == status_in_progress)
			++in_progress;
		else
			++completed;
	}
	co_return text_result(
		"Updated todo list: " + std::to_string(pending) + " pending, " + std::to_string(in_progress) +
		" in progress, " + std::to_string(completed) + " completed.");
}

struct tool_todo_plugin : araya::plugin {
	explicit tool_todo_plugin(araya::plugin_config config) {
		araya::plugin_config_view const view(config);
		if (auto value = view.try_get(allow_parallel_key))
			allow_parallel_ = *value;
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<tools_service>(tools_key).shared();
		auto store = ctx.require<session_store>(sessions_key).shared();

		// Standing-plan fold: the latest whole `todo/write` list, cleared by
		// the next `turn/start` (turn/end keeps the finished checklist).
		store->register_projection(
			ctx,
			event_projection<todo_state>{
				.name = "todos",
				.types = {"todo/write", "turn/start"},
				.init = [](session_header const&) -> todo_state { return std::nullopt; },
				.apply =
					[](todo_state& state, session_event const& event) {
						if (event.type == "turn/start") {
							state.reset();
							return;
						}
						auto const* object = event.data.if_object();
						auto const* items = object ? araya::util::json::get_array(*object, "todos") : nullptr;
						todo_list list;
						if (items) {
							for (auto const& entry : *items) {
								auto const* item = entry.if_object();
								if (!item)
									continue;
								list.push_back(todo_item{
									araya::util::json::get_string(*item, "content"),
									araya::util::json::get_string(*item, "status")});
							}
						}
						state = std::move(list);
					},
			},
			todos_);

		auto impl = std::make_shared<todos_impl>();
		impl->tracker = &todos_;
		ctx.provide(todos_key, std::shared_ptr<todos_service>(std::move(impl)));

		auto allow_parallel = allow_parallel_;
		tools->register_tool(
			ctx,
			tool_definition{"todo_write", describe(allow_parallel), todos_schema()},
			[store, allow_parallel](tool_context const& call) { return handle_todo(store, allow_parallel, call); });
		co_return;
	}

private:
	bool allow_parallel_ = true;
	projection_state<todo_state> todos_;
};

std::unique_ptr<araya::plugin> make_tool_todo(araya::plugin_config const& config) {
	return std::make_unique<tool_todo_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"todos", 1}}};
static const araya::plugin_descriptor g_descriptor{"tool-todo", g_deps, g_provs, &make_tool_todo};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_todo
