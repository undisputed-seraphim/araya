#include "app_core.hpp"
#include "commands.hpp"

#include "demo_plugins.hpp"

#include "araya/agent-instructions/agent_instructions.hpp"
#include "araya/agent-loop/agent.hpp"
#include "araya/coreutil/coreutil.hpp"
#include "araya/fs-observation-policy/fs_observation_policy.hpp"
#include "araya/jobs/jobs.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm-openai/openai.hpp"
#include "araya/llm/llm.hpp"
#include "araya/logger/logger.hpp"
#include "araya/persistence/persistence.hpp"
#include "araya/session/events.hpp"
#include "araya/shell/shell.hpp"
#include "araya/subagents/subagents.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/timer/timer.hpp"
#include "araya/tool-jobs/tool_jobs.hpp"
#include "araya/tool-subagent/tool_subagent.hpp"
#include "araya/tool-todo/tool_todo.hpp"
#include "araya/tools/tools.hpp"

#include <boost/asio/use_awaitable.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace araya::app {
namespace {

using namespace std::chrono_literals;

using araya::logger::logger_key;
using araya::logger::logger_service;
using araya::timer::timer_key;
using araya::timer::timer_service;

// -- helpers -------------------------------------------------------------

araya::logger::log_level parse_level(std::string_view word) {
	if (word == "error")
		return araya::logger::log_level::error;
	if (word == "warn")
		return araya::logger::log_level::warn;
	if (word == "info")
		return araya::logger::log_level::info;
	if (word == "debug")
		return araya::logger::log_level::debug;
	throw std::invalid_argument("level is one of error, warn, info, debug");
}

// The app's demo tool: registered at boot so the scripted agent demo has
// something to call. Real hosts register their own tools from plugins.
araya::task<araya::tools::tool_result> demo_echo(araya::tools::tool_context const& ctx) {
	std::string text;
	if (auto const* object = ctx.arguments.if_object()) {
		if (auto const* node = object->if_contains("text"); node && node->is_string())
			text = std::string(node->as_string());
	}
	co_return araya::tools::tool_result{boost::json::array{{{"type", "text"}, {"text", "echo tool: " + text}}}, false};
}

// -- the misc commands (timer, log) ---------------------------------------

araya::task<void> cmd_timer(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		if (sub == "in" || sub == "every") {
			double seconds = 0.0;
			is >> seconds;
			std::string text;
			std::getline(is, text);
			text = trim(text);
			if (!is || text.empty()) {
				out("timer: usage: timer " + sub + " <seconds> <text>");
			} else {
				auto root_ctx = ctx.rt->root_context();
				auto timer = root_ctx.require<timer_service>(timer_key).shared();
				auto delay =
					std::chrono::duration_cast<timer_service::duration>(std::chrono::duration<double>(seconds));
				if (sub == "in") {
					(void)timer->timeout(root_ctx, delay, [out, text] { out("timer: " + text); });
					out("timer: armed");
				} else {
					auto reg = timer->interval(root_ctx, delay, [out, text] {
						out("timer: " + text);
						return true;
					});
					ctx.intervals.push_back(std::move(reg));
					out("timer: interval armed");
				}
			}
		} else if (sub == "cancel") {
			for (auto& reg : ctx.intervals)
				reg.release();
			ctx.intervals.clear();
			out("timer: intervals cancelled");
		} else {
			out("timer: in | every | cancel");
		}
	} catch (std::exception const& e) {
		out(std::string("timer: ") + e.what());
	}
	co_return;
}

void log_set_level(logger_service& logger, std::istream& is, line_sink const& out) {
	std::string level;
	is >> level;
	if (level.empty()) {
		out("log: usage: log level <level>");
		return;
	}
	logger.set_level(parse_level(level));
	out("log: level set");
}

void log_emit(logger_service& logger, std::string const& level, std::istream& is, line_sink const& out) {
	std::string text;
	std::getline(is, text);
	text = trim(text);
	if (text.empty()) {
		out("log: usage: log <level> <text>");
		return;
	}
	auto named = logger.named("main");
	switch (parse_level(level)) {
	case araya::logger::log_level::error:
		named.error("{}", text);
		break;
	case araya::logger::log_level::warn:
		named.warn("{}", text);
		break;
	case araya::logger::log_level::info:
		named.info("{}", text);
		break;
	case araya::logger::log_level::debug:
		named.debug("{}", text);
		break;
	}
	out("log: submitted");
}

araya::task<void> cmd_log(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;

		auto logger = ctx.rt->root_context().require<logger_service>(logger_key).shared();
		if (sub == "level")
			log_set_level(*logger, is, out);
		else
			log_emit(*logger, sub, is, out);
	} catch (std::exception const& e) {
		out(std::string("log: ") + e.what());
	}
	co_return;
}

// -- the command table ----------------------------------------------------

araya::task<void> cmd_prompt(app_context& ctx, line_sink const& out, std::string const& line) {
	try {
		auto prompts =
			ctx.rt->root_context()
				.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
				.shared();
		std::istringstream is(line);
		std::string cmd;
		std::string sub;
		is >> cmd >> sub;
		if (sub.empty() || sub == "show") {
			out("prompt: prefix \"" + prompts->persona_prefix() + "\"");
			out("prompt: suffix \"" + prompts->persona_suffix() + "\"");
		} else if (sub == "set") {
			std::string text;
			std::getline(is, text);
			prompts->set_persona_prefix(trim(text));
			out("prompt: prefix set");
		} else if (sub == "suffix") {
			std::string text;
			std::getline(is, text);
			prompts->set_persona_suffix(trim(text));
			out("prompt: suffix set");
		} else if (sub == "clear") {
			prompts->set_persona_prefix("");
			prompts->set_persona_suffix("");
			out("prompt: cleared");
		} else if (sub == "identity") {
			std::string value;
			is >> value;
			if (value != "on" && value != "off") {
				out("prompt: usage: prompt identity on|off");
				co_return;
			}
			prompts->set_include_harness_identity(value == "on");
			out(std::string("prompt: identity ") + value);
		} else {
			out("prompt: show | set <text> | suffix <text> | identity on|off | clear");
		}
	} catch (std::exception const& e) {
		out(std::string("prompt: ") + e.what());
	}
	co_return;
}

struct command_entry {
	std::string_view name;
	std::string_view usage;
	std::string_view summary;
	command_fn run;
};

constexpr command_entry g_commands[]{
	{"ls", "ls", "print the fiber tree", &cmd_ls},
	{"load",
	 "load <component> [json config]",
	 "add logger | timer | session | persistence | llm | llm-openai | llm-mock | system-prompt | "
	 "agent-instructions | tools | tool-todo | agent-loop | coreutil | fs-observation-policy | jobs | shell | "
	 "tool-jobs | subagents | tool-subagent | console | beacon | watcher",
	 &cmd_load},
	{"unload", "unload <component>", "retire it (watch the cascade)", &cmd_unload},
	{"reload", "reload <component>", "retire and remount it", &cmd_reload},
	{"fail", "fail <component>", "swap in a crashing provider (logger | timer | session)", &cmd_fail},
	{"avail", "avail wait | ready", "park the watcher on the beacon, then promote it", &cmd_avail},
	{"session",
	 "session ...",
	 "new | switch | list | append | replace | fork | show | save | load | close",
	 &cmd_session},
	{"timer", "timer ...", "in <sec> <text> | every <sec> <text> | cancel", &cmd_timer},
	{"log", "log ...", "level <level> | <level> <text>", &cmd_log},
	{"prompt", "prompt ...", "show | set <text> | suffix <text> | identity on|off | clear", &cmd_prompt},
	{"tool", "tool", "list the agent's registered tools", &cmd_tool},
	{"ask", "ask <text>", "run the agent loop (tools + system prompt)", &cmd_ask},
	{"chat", "chat <text>", "one-shot prompt through the llm service", &cmd_chat},
	{"say", "say <text>", "append a message locally (no model)", &cmd_say},
};

} // namespace

// -- public API -----------------------------------------------------------

std::string trim(std::string_view text) {
	auto first = text.find_first_not_of(" \t");
	if (first == std::string_view::npos)
		return {};
	auto last = text.find_last_not_of(" \t");
	return std::string(text.substr(first, last - first + 1));
}

std::shared_ptr<araya::plugin_descriptor> borrow(araya::plugin_descriptor const& d) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(&d), [](araya::plugin_descriptor*) {});
}

std::vector<araya::desired_component> make_desired(app_context& ctx) {
	std::vector<araya::desired_component> result;
	result.reserve(ctx.desired.size());
	for (auto const& [path, entry] : ctx.desired)
		result.push_back(araya::desired_component{
			path,
			araya::component_spec{borrow(*entry.descriptor), entry.config, nullptr, path, {}},
		});
	return result;
}

araya::plugin_descriptor const* real_descriptor(std::string_view name) {
	if (name == "logger")
		return &araya::logger::plugin_descriptor();
	if (name == "timer")
		return &araya::timer::plugin_descriptor();
	if (name == "session")
		return &araya::session::plugin_descriptor();
	if (name == "persistence")
		return &araya::persistence::plugin_descriptor();
	if (name == "llm")
		return &araya::llm::plugin_descriptor();
	if (name == "llm-openai")
		return &araya::llm_openai::plugin_descriptor();
	if (name == "llm-mock")
		return &araya::llm_mock::plugin_descriptor();
	if (name == "system-prompt")
		return &araya::system_prompt::plugin_descriptor();
	if (name == "agent-instructions")
		return &araya::agent_instructions::plugin_descriptor();
	if (name == "tools")
		return &araya::tools::plugin_descriptor();
	if (name == "tool-todo")
		return &araya::tool_todo::plugin_descriptor();
	if (name == "agent-loop")
		return &araya::agent::plugin_descriptor();
	if (name == "coreutil")
		return &araya::coreutil::plugin_descriptor();
	if (name == "fs-observation-policy")
		return &araya::fs_observation_policy::plugin_descriptor();
	if (name == "jobs")
		return &araya::jobs::plugin_descriptor();
	if (name == "shell")
		return &araya::shell::plugin_descriptor();
	if (name == "tool-jobs")
		return &araya::tool_jobs::plugin_descriptor();
	if (name == "subagents")
		return &araya::subagents::plugin_descriptor();
	if (name == "tool-subagent")
		return &araya::tool_subagent::plugin_descriptor();
	if (name == "console")
		return &araya::console_demo::console_descriptor();
	if (name == "beacon")
		return &araya::console_demo::beacon_descriptor();
	if (name == "watcher")
		return &araya::console_demo::watcher_descriptor();
	return nullptr;
}

araya::plugin_config default_config(std::string_view name) {
	if (name == "logger")
		return {{"name", "araya"}, {"level", "info"}};
	if (name == "persistence")
		return {{"root", "araya-sessions"}};
	return {};
}

char const* state_name(araya::fiber_state s) {
	static constexpr std::array<char const*, 4> names{"inactive", "loading", "active", "unloading"};
	auto const index = std::to_underlying(s);
	return index < names.size() ? names[index] : "?";
}

std::string error_text(std::exception_ptr ep) {
	if (!ep)
		return {};
	try {
		std::rethrow_exception(ep);
	} catch (std::exception const& e) {
		return e.what();
	} catch (...) {
		return "unknown error";
	}
}

std::string_view role_name(araya::session::message_role role) {
	static constexpr std::array<std::string_view, 4> names{"system", "user", "assistant", "tool"};
	auto const index = std::to_underlying(role);
	return index < names.size() ? names[index] : std::string_view("?");
}

std::string cwd_branch_line(std::string const& cwd) {
	std::string shown = cwd;
	if (auto home = std::getenv("HOME"); home && !shown.rfind(home, 0)) {
		shown = "~" + shown.substr(std::strlen(home));
	}
	std::string branch;
	std::string command = "git -C \"" + cwd + "\" symbolic-ref --short HEAD 2>/dev/null";
	if (FILE* pipe = ::popen(command.c_str(), "r")) {
		char buffer[128] = {};
		if (std::fgets(buffer, sizeof buffer, pipe)) {
			branch = buffer;
			while (!branch.empty() && (branch.back() == '\n' || branch.back() == '\r'))
				branch.pop_back();
		}
		::pclose(pipe);
	}
	return branch.empty() ? shown : shown + ":" + branch;
}

araya::task<void> boot(app_context& ctx, line_sink const& out) {
	ctx.desired["logger"] = desired_entry{&araya::logger::plugin_descriptor(), {{"name", "araya"}, {"level", "info"}}};
	ctx.desired["timer"] = desired_entry{&araya::timer::plugin_descriptor(), {}};
	ctx.desired["session"] = desired_entry{&araya::session::plugin_descriptor(), {}};
	ctx.desired["persistence"] = desired_entry{&araya::persistence::plugin_descriptor(), {{"root", "araya-sessions"}}};
	ctx.desired["llm"] = desired_entry{&araya::llm::plugin_descriptor(), {}};
	// The prompt and tool registries the agent loop assembles from. The
	// system prompt's persona is config-only - nothing is hardcoded; the
	// ARAYA_SYSTEM_PROMPT environment variable seeds the prefix.
	araya::plugin_config prompt_config;
	if (auto const* env = std::getenv("ARAYA_SYSTEM_PROMPT"); env && *env) {
		prompt_config["persona_prefix"] = env;
	} else {
		// The standard coding-agent persona (the harness's `standard`
		// preset). The environment variable still wins when set.
		prompt_config["persona_prefix"] = "You are a coding agent powered by the {{model}} model.";
		prompt_config["persona_suffix"] = "Your working directory is {{cwd}}.";
	}
	ctx.desired["system-prompt"] = desired_entry{&araya::system_prompt::plugin_descriptor(), std::move(prompt_config)};
	// Workspace instructions (AGENTS.md) as a system-prompt section.
	ctx.desired["agent-instructions"] = desired_entry{&araya::agent_instructions::plugin_descriptor(), {}};
	ctx.desired["tools"] = desired_entry{&araya::tools::plugin_descriptor(), {}};
	ctx.desired["tool-todo"] = desired_entry{&araya::tool_todo::plugin_descriptor(), {}};
	ctx.desired["agent-loop"] = desired_entry{&araya::agent::plugin_descriptor(), {}};
	// Background jobs: the registry, then the model-facing controls whose
	// load attaches the controller producers need.
	ctx.desired["jobs"] = desired_entry{&araya::jobs::plugin_descriptor(), {}};
	ctx.desired["tool-jobs"] = desired_entry{&araya::tool_jobs::plugin_descriptor(), {}};
	// The built-in file/search tools and the one-shot shell tool. The
	// shell resolves relative workdirs against the session cwd; its
	// run_in_background path registers with the jobs registry.
	ctx.desired["coreutil"] = desired_entry{&araya::coreutil::plugin_descriptor(), {}};
	ctx.desired["shell"] = desired_entry{&araya::shell::plugin_descriptor(), {}};
	// The read-before-write/edit policy: an event-only gate the coreutil
	// executor consults. Without it the fs tools are unconstrained.
	ctx.desired["fs-observation-policy"] = desired_entry{&araya::fs_observation_policy::plugin_descriptor(), {}};
	// Delegation: the subagent seam (spawn provider + continuable
	// children) and the model-facing tools. The child route is inherited
	// from the parent's latest request header unless configured here.
	ctx.desired["subagents"] = desired_entry{&araya::subagents::plugin_descriptor(), {}};
	ctx.desired["tool-subagent"] = desired_entry{&araya::tool_subagent::plugin_descriptor(), {}};
	if (!ctx.llm_config.empty())
		ctx.desired["llm-openai"] =
			desired_entry{&araya::llm_openai::plugin_descriptor(), {{"config_file", ctx.llm_config}}};
	ctx.desired["beacon"] = desired_entry{&araya::console_demo::beacon_descriptor(), {}};
	ctx.desired["watcher"] = desired_entry{&araya::console_demo::watcher_descriptor(), {}};
	ctx.desired["console"] = desired_entry{&araya::console_demo::console_descriptor(), {}};

	co_await ctx.rt->reconcile(make_desired(ctx));
	co_await ctx.rt->wait_idle();

	std::error_code cwd_ec;
	ctx.cwd = std::filesystem::current_path(cwd_ec).string();
	if (cwd_ec)
		ctx.cwd = "?";
	ctx.cwd_branch = cwd_branch_line(ctx.cwd);

	// The live feed: host-owned listeners on the session firehose. They
	// live until the bus dies with the runtime, and write as events land.
	co_await ctx.rt->run_on_strand([&] {
		auto* bus = ctx.rt->bus().get();
		bus->add_listener(
			araya::session::created_key,
			[out](araya::session::session_created_msg const& m) { out("event: session/created " + m.s->id().value); },
			0);
		bus->add_listener(
			araya::session::disposed_key,
			[out](araya::session::session_disposed_msg const& m) { out("event: session/disposed " + m.id.value); },
			0);
		bus->add_listener(
			araya::session::appended_key,
			[out](araya::session::session_appended_msg const& m) {
				out("event: session/event " + m.id.value + " seq " + std::to_string(m.event.seq) + " type " +
					m.event.type);
			},
			0);

		// The app's demo tool, registered on the host fiber so the
		// scripted agent demo has something to call. Real hosts register
		// their tools from plugins.
		auto root = ctx.rt->root_context();
		auto tools = root.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		tools->register_tool(
			root,
			araya::tools::tool_definition{
				"echo",
				"echoes the text argument",
				boost::json::value{
					{"type", "object"},
					{"properties",
					 boost::json::object{
						 {"text", boost::json::object{{"type", "string"}, {"description", "the text to echo"}}}}},
					{"required", boost::json::array{"text"}}}},
			&demo_echo);
	});
}

araya::task<void> dispatch(app_context& ctx, line_sink const& out, std::string_view line) {
	auto trimmed = trim(line);
	if (trimmed.empty())
		co_return;
	if (trimmed.front() != '/') {
		out("commands start with '/' (try /help)");
		co_return;
	}
	std::string_view body(trimmed);
	body.remove_prefix(1);
	auto first = body.find_first_not_of(" \t");
	if (first == std::string_view::npos) {
		out("commands start with '/' (try /help)");
		co_return;
	}
	body = body.substr(first);
	auto name = body.substr(0, body.find_first_of(" \t"));

	if (name == "help") {
		out(help_text());
		co_return;
	}
	std::string body_str(body);
	for (auto const& entry : g_commands) {
		if (entry.name == name) {
			co_await entry.run(ctx, out, body_str);
			co_return;
		}
	}
	out("unknown command '/" + std::string(name) + "' (try /help)");
}

std::string help_text() {
	std::size_t usage_width = 0;
	for (auto const& entry : g_commands)
		usage_width = std::max(usage_width, entry.usage.size());
	std::string text = "commands:\n";
	for (auto const& entry : g_commands) {
		text += "  /" + std::string(entry.usage);
		text += std::string(usage_width - entry.usage.size() + 2, ' ');
		text += std::string(entry.summary) + "\n";
	}
	text += "  /help                   this text";
	return text;
}

std::span<araya::app::command_info const> command_list() {
	static std::vector<araya::app::command_info> const list = [] {
		std::vector<araya::app::command_info> out;
		out.push_back({"help", "help", "this text"});
		for (auto const& entry : g_commands)
			out.push_back({entry.name, entry.usage, entry.summary});
		out.push_back({"quit", "quit", "exit the app"});
		return out;
	}();
	return list;
}

std::optional<active_model> active_route(app_context& ctx, std::string* why) {
	auto lease = ctx.rt->root_context().find<araya::llm::llm_service>(araya::llm::llm_key);
	if (!lease) {
		if (why)
			*why = "llm service is not mounted";
		return std::nullopt;
	}
	auto service = lease->shared();
	auto provider = service->first_provider();
	if (!provider) {
		if (why)
			*why = "llm service has no provider routes (load llm-mock or configure llm-openai)";
		return std::nullopt;
	}
	auto model = service->resolve_model(*provider, "");
	if (!model || model->model.empty()) {
		if (why)
			*why = "provider '" + std::string(*provider) + "' did not resolve a default model";
		return std::nullopt;
	}
	return active_model{std::string(*provider), model->model, model->name, model->context_window};
}

} // namespace araya::app
