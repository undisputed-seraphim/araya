#include "araya/tool-lsp/tool_lsp.hpp"

#include "araya/config.hpp"
#include "araya/lsp/lsp.hpp"
#include "araya/plugin_context.hpp"
#include "araya/session/session_types.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::tool_lsp {
namespace {

using araya::lsp::lsp_key;
using araya::lsp::lsp_location;
using araya::lsp::lsp_operation;
using araya::lsp::lsp_query_request;
using araya::lsp::lsp_query_result;
using araya::lsp::lsp_service;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::tools::error_result;
using araya::tools::text_result;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr araya::config_key<std::uint64_t> max_locations_key{"maxLocations"};
constexpr araya::config_key<std::uint64_t> max_result_chars_key{"maxResultChars"};
constexpr araya::config_key<std::uint64_t> timeout_ms_key{"timeoutMs"};

constexpr araya::config_field g_config[] = {
	field(max_locations_key, "Largest number of rendered locations before an omission marker.", "100"),
	field(max_result_chars_key, "Largest complete rendered result in characters.", "16000"),
	field(timeout_ms_key, "Per-call timeout in milliseconds.", "60000"),
};

constexpr std::string_view lsp_prompt_text =
	"Use search/read for ordinary navigation. Use lsp when textual matches are ambiguous or before a change "
	"requires precise definitions, implementations, or references. Positions are one-based line and character "
	"(UTF-16) at the cursor; an off-symbol position may return no results. findReferences always includes the "
	"declaration.";

struct tool_lsp_config {
	std::size_t max_locations = 100;
	std::size_t max_result_chars = 16'000;
	std::uint64_t timeout_ms = 60'000;
};

tool_lsp_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view const view(config);
	tool_lsp_config out;
	if (auto value = view.try_get(max_locations_key))
		out.max_locations = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(max_result_chars_key))
		out.max_result_chars = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(timeout_ms_key))
		out.timeout_ms = *value;
	return out;
}

std::optional<lsp_operation> parse_operation(std::string const& name) {
	if (name == "goToDefinition")
		return lsp_operation::go_to_definition;
	if (name == "findReferences")
		return lsp_operation::find_references;
	if (name == "goToImplementation")
		return lsp_operation::go_to_implementation;
	if (name == "hover")
		return lsp_operation::hover;
	return std::nullopt;
}

std::string decode_file_uri(std::string_view uri) {
	if (uri.rfind("file://", 0) != 0)
		return {};
	std::string out;
	std::string_view const rest = uri.substr(7);
	for (std::size_t i = 0; i < rest.size(); ++i) {
		if (rest[i] == '%' && i + 2 < rest.size()) {
			auto hex = [](char c) -> int {
				if (c >= '0' && c <= '9')
					return c - '0';
				if (c >= 'a' && c <= 'f')
					return c - 'a' + 10;
				if (c >= 'A' && c <= 'F')
					return c - 'A' + 10;
				return -1;
			};
			int const hi = hex(rest[i + 1]);
			int const lo = hex(rest[i + 2]);
			if (hi >= 0 && lo >= 0) {
				out.push_back(static_cast<char>((hi << 4) | lo));
				i += 2;
				continue;
			}
		}
		out.push_back(rest[i]);
	}
	return out;
}

std::string render_uri(std::string uri, std::string const& workspace_uri) {
	if (uri.rfind("file:", 0) != 0)
		return uri;
	std::string const target = decode_file_uri(uri);
	std::string const workspace = decode_file_uri(workspace_uri);
	if (target.empty() || workspace.empty())
		return uri;
	std::filesystem::path const target_path(target);
	std::filesystem::path const workspace_path(workspace);
	std::error_code ec;
	auto const relative = std::filesystem::relative(target_path, workspace_path, ec);
	if (ec)
		return target;
	std::string const rendered = relative.string();
	if (rendered.empty() || rendered == ".")
		return ".";
	if (rendered.rfind("..", 0) == 0)
		return target;
	return rendered;
}

std::string bound_result(std::string text, std::size_t max_chars, std::string_view label) {
	if (text.size() <= max_chars)
		return text;
	std::string const notice =
		"\n… " + std::string(label) + " truncated (limit " + std::to_string(max_chars) + " characters).";
	if (notice.size() >= max_chars)
		return notice.substr(0, max_chars);
	return text.substr(0, max_chars - notice.size()) + notice;
}

std::string format_locations(
	std::vector<lsp_location> const& locations,
	std::string const& workspace_uri,
	std::size_t max_locations,
	std::size_t max_result_chars) {
	if (locations.empty())
		return bound_result("No results.", max_result_chars, "locations");
	std::size_t const shown = std::min(locations.size(), max_locations);
	std::map<std::string, std::vector<std::string>> grouped;
	for (std::size_t i = 0; i < shown; ++i) {
		auto const& location = locations[i];
		std::string const path = render_uri(location.uri, workspace_uri);
		grouped[path].push_back(
			path + ":" + std::to_string(location.range.start.line + 1) + ":" +
			std::to_string(location.range.start.character + 1));
	}
	std::vector<std::string> lines;
	for (auto const& [path, entries] : grouped)
		for (auto const& entry : entries)
			lines.push_back(entry);
	std::size_t const omitted = locations.size() - shown;
	if (omitted > 0)
		lines.push_back(
			"… " + std::to_string(omitted) + " more location" + (omitted == 1 ? "" : "s") + " omitted (limit " +
			std::to_string(max_locations) + ").");
	std::string text;
	for (std::size_t i = 0; i < lines.size(); ++i) {
		if (i)
			text += "\n";
		text += lines[i];
	}
	return bound_result(std::move(text), max_result_chars, "locations");
}

std::string format_hover(std::optional<araya::lsp::lsp_hover> const& hover, std::size_t max_result_chars) {
	return bound_result(
		hover.has_value() ? hover->contents : std::string{"No hover information."}, max_result_chars, "hover");
}

araya::task<tool_result> handle(
	std::shared_ptr<lsp_service> lsp,
	std::shared_ptr<session_store> sessions,
	tool_lsp_config config,
	tool_context const& call) {
	auto const* args = call.arguments.if_object();
	if (!args)
		co_return error_result("Error: the lsp tool requires an object of arguments");
	std::string const operation_name = args ? araya::util::json::get_string(*args, "operation") : std::string{};
	auto const operation = parse_operation(operation_name);
	if (!operation)
		co_return error_result(
			"Error: operation must be one of goToDefinition, findReferences, goToImplementation, hover");
	std::string const file_path = araya::util::json::get_string(*args, "file_path");
	if (file_path.empty())
		co_return error_result("Error: file_path must be a non-empty string");
	auto const line = araya::util::json::opt_int(*args, "line");
	auto const character = araya::util::json::opt_int(*args, "character");
	if (!line || *line < 1)
		co_return error_result("Error: line must be a positive integer (one-based)");
	if (!character || *character < 1)
		co_return error_result("Error: character must be a positive integer (one-based)");

	std::string workspace;
	if (!call.session.empty()) {
		if (auto session = sessions->get(session_id{call.session}); session && session->header().cwd)
			workspace = *session->header().cwd;
	}
	if (workspace.empty())
		co_return error_result("Error: the lsp tool requires a session workspace cwd");

	lsp_query_request request;
	request.operation = *operation;
	request.file_path = file_path;
	request.position = {static_cast<std::uint32_t>(*line - 1), static_cast<std::uint32_t>(*character - 1)};
	request.workspace_root = workspace;

	using namespace boost::asio::experimental::awaitable_operators;
	boost::asio::steady_timer timer(co_await boost::asio::this_coro::executor);
	timer.expires_after(std::chrono::milliseconds(config.timeout_ms));
	try {
		auto outcome = co_await (lsp->query(request, call.stop) || timer.async_wait(boost::asio::use_awaitable));
		if (outcome.index() == 1)
			co_return error_result("Error: lsp query timed out after " + std::to_string(config.timeout_ms) + " ms");
		lsp_query_result const result = std::get<0>(std::move(outcome));
		if (result.type == lsp_query_result::kind::hover)
			co_return text_result(format_hover(result.hover, config.max_result_chars));
		co_return text_result(format_locations(
			result.locations, result.resolved_workspace_uri, config.max_locations, config.max_result_chars));
	} catch (araya::lsp::lsp_error const& error) {
		co_return error_result("Error: " + std::string(error.what()) + " (" + error.code() + ")");
	} catch (std::exception const& error) {
		co_return error_result(std::string("Error: ") + error.what());
	}
}

boost::json::value lsp_schema() {
	boost::json::object operation;
	operation["type"] = "string";
	operation["description"] = "goToDefinition, findReferences, goToImplementation, or hover.";
	operation["enum"] = boost::json::array{"goToDefinition", "findReferences", "goToImplementation", "hover"};
	boost::json::object file_path;
	file_path["type"] = "string";
	file_path["description"] = "The source file to query, relative to the workspace or absolute.";
	boost::json::object line;
	line["type"] = "integer";
	line["description"] = "One-based line of the cursor.";
	boost::json::object character;
	character["type"] = "integer";
	character["description"] = "One-based UTF-16 column of the cursor.";
	boost::json::object properties;
	properties["operation"] = std::move(operation);
	properties["file_path"] = std::move(file_path);
	properties["line"] = std::move(line);
	properties["character"] = std::move(character);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = boost::json::array{"operation", "file_path", "line", "character"};
	return schema;
}

std::unique_ptr<araya::plugin> make_tool_lsp(araya::plugin_config const& config) {
	struct tool_lsp_plugin : araya::plugin {
		explicit tool_lsp_plugin(araya::plugin_config const& cfg)
			: config(parse_config(cfg)) {}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto lsp = ctx.require<lsp_service>(lsp_key).shared();
			auto sessions = ctx.require<session_store>(sessions_key).shared();
			auto prompts =
				ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key)
					.shared();
			auto tools = ctx.require<tools_service>(tools_key).shared();

			araya::system_prompt::prompt_section section;
			section.name = "tool:lsp";
			section.order = araya::system_prompt::section_order("TOOL_LSP");
			section.text = std::string(lsp_prompt_text);
			prompts->section(ctx, std::move(section));

			auto resolved = config;
			auto lsp_handle = std::shared_ptr<lsp_service>{lsp};
			auto sessions_handle = std::shared_ptr<session_store>{sessions};
			tools->register_tool(
				ctx,
				tool_definition{
					"lsp",
					"Query a language server for precise code navigation. operation is one of goToDefinition, "
					"findReferences, goToImplementation, hover. line and character are one-based UTF-16 cursor "
					"coordinates. findReferences includes the declaration.",
					lsp_schema()},
				[lsp_handle, sessions_handle, resolved](tool_context const& call) {
					return handle(lsp_handle, sessions_handle, resolved, call);
				});
			co_return;
		}

		tool_lsp_config config;
	};
	return std::make_unique<tool_lsp_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"lsp", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-lsp", g_deps, g_provs, &make_tool_lsp, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_lsp
