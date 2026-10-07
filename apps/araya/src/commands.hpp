#pragma once

#include "app_core.hpp"

#include <string>
#include <string_view>

// The command implementations, split across the surface-agnostic
// translation units (commands_components / commands_session /
// commands_llm; timer and log stay in app_core.cpp). App-internal:
// never installed.
namespace araya::app {

using command_fn = araya::task<void> (*)(app_context&, line_sink const&, std::string const&);

araya::task<void> cmd_ls(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_load(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_unload(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_reload(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_config(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_fail(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_avail(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_session(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_chat(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_ask(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_tool(app_context& ctx, line_sink const& out, std::string const& line);
// Invoke one registered tool directly with JSON arguments, bypassing the
// model. Script/testing affordance so a script can exercise every tool
// plugin deterministically.
araya::task<void> cmd_call(app_context& ctx, line_sink const& out, std::string const& line);
// Print a process memory snapshot (RSS/VM from /proc plus glibc arena stats);
// `mem dump <path>` writes the malloc_info XML for offline analysis.
araya::task<void> cmd_mem(app_context& ctx, line_sink const& out, std::string const& line);
// The human goal surface (inspect/create/pause/resume/complete/clear).
araya::task<void> cmd_goal(app_context& ctx, line_sink const& out, std::string const& line);
// A local conversation turn: appends a user message to the current
// session (auto-created) without touching the llm service. Plain-text
// prompt input routes here.
araya::task<void> cmd_say(app_context& ctx, line_sink const& out, std::string const& line);

// Whitespace trimming, shared by the command parsers.
std::string trim(std::string_view text);

} // namespace araya::app
