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
araya::task<void> cmd_fail(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_avail(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_session(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_chat(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_ask(app_context& ctx, line_sink const& out, std::string const& line);
araya::task<void> cmd_tool(app_context& ctx, line_sink const& out, std::string const& line);

// Whitespace trimming, shared by the command parsers.
std::string trim(std::string_view text);

} // namespace araya::app
