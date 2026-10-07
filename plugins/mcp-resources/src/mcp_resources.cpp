#include "araya/mcp-resources/mcp_resources.hpp"

#include "araya/mcp-client/mcp.hpp"
#include "araya/plugin_context.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::mcp_resources {
namespace {

using araya::mcp::mcp_key;
using araya::mcp::mcp_service;
using araya::system_prompt::assemble_context;
using araya::system_prompt::prompt_section;
using araya::system_prompt::system_prompt_key;
using araya::system_prompt::system_prompt_service;
using araya::tools::error_result;
using araya::tools::text_result;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr std::string_view binary_prefix = "[binary resource: ";
constexpr std::string_view binary_suffix = " base64 characters; available to programmatic callers]";

// Replaces string-valued `blob` fields with a length description, leaving
// every other field (uri, mimeType, text, cursors) intact. Mirrors the DSH
// resource renderer's pure text projection.
boost::json::value project_blobs(boost::json::value const& value) {
	if (auto const* object = value.if_object()) {
		boost::json::object out;
		for (auto const& [key, entry] : *object) {
			if (key == "blob" && entry.is_string())
				out[key] =
					std::string(binary_prefix) + std::to_string(entry.as_string().size()) + std::string(binary_suffix);
			else
				out[key] = project_blobs(entry);
		}
		return out;
	}
	if (auto const* array = value.if_array()) {
		boost::json::array out;
		out.reserve(array->size());
		for (auto const& entry : *array)
			out.push_back(project_blobs(entry));
		return out;
	}
	return value;
}

std::string render_result(std::string const& server, boost::json::value const& result) {
	return "MCP server: " + server + "\n" + boost::json::serialize(project_blobs(result));
}

araya::task<tool_result>
run_resource(std::shared_ptr<mcp_service> service, std::string method, tool_context const& call) {
	auto const* args = call.arguments.if_object();
	std::string const server = args ? araya::util::json::get_string(*args, "server") : std::string{};
	if (server.empty())
		co_return error_result("Error: the 'server' argument is required");
	boost::json::object params;
	if (method == "resources/read") {
		std::string const uri = args ? araya::util::json::get_string(*args, "uri") : std::string{};
		if (uri.empty())
			co_return error_result("Error: the 'uri' argument is required");
		params["uri"] = uri;
	} else if (args) {
		if (auto const cursor = araya::util::json::get_string(*args, "cursor"); !cursor.empty())
			params["cursor"] = cursor;
	}
	try {
		boost::json::value const result = co_await service->request(server, method, std::move(params), call.stop);
		co_return text_result(render_result(server, result));
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}
}

boost::json::value server_parameter() {
	boost::json::object server;
	server["type"] = "string";
	server["description"] = "Configured MCP server name.";
	return server;
}

boost::json::value list_schema() {
	boost::json::object cursor;
	cursor["type"] = "string";
	cursor["description"] = "Continuation cursor returned by this server.";
	boost::json::object properties;
	properties["server"] = server_parameter();
	properties["cursor"] = std::move(cursor);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = boost::json::array{"server"};
	return schema;
}

boost::json::value read_schema() {
	boost::json::object uri;
	uri["type"] = "string";
	uri["description"] = "Resource URI to read.";
	boost::json::object properties;
	properties["server"] = server_parameter();
	properties["uri"] = std::move(uri);
	boost::json::object schema;
	schema["type"] = "object";
	schema["properties"] = std::move(properties);
	schema["required"] = boost::json::array{"server", "uri"};
	return schema;
}

std::string servers_section(std::shared_ptr<mcp_service> const& service) {
	std::vector<std::string> names = service->servers();
	if (names.empty())
		return {};
	std::sort(names.begin(), names.end());
	boost::json::array as_json;
	for (auto const& name : names)
		as_json.push_back(boost::json::value(name));
	return "## MCP resource servers\n\nUse list_mcp_resources, list_mcp_resource_templates, or read_mcp_resource "
		   "with one of these names as the server argument: " +
		   boost::json::serialize(as_json) + ".";
}

std::unique_ptr<araya::plugin> make_mcp_resources(araya::plugin_config const&) {
	struct mcp_resources_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto service = ctx.require<mcp_service>(mcp_key).shared();
			auto tools = ctx.require<tools_service>(tools_key).shared();
			auto prompts = ctx.require<system_prompt_service>(system_prompt_key).shared();

			if (service->servers().empty())
				co_return;

			prompt_section section;
			section.name = "mcp-resource-servers";
			section.order = araya::system_prompt::section_order("MCP_SERVERS");
			section.interpolate = false;
			auto weak_service = std::weak_ptr<mcp_service>(service);
			section.render = [weak_service](assemble_context const&) {
				auto live = weak_service.lock();
				return live ? servers_section(live) : std::string{};
			};
			prompts->section(ctx, std::move(section));

			auto shared = std::shared_ptr<mcp_service>{service};
			tools->register_tool(
				ctx,
				tool_definition{"list_mcp_resources", "List resources available from an MCP server.", list_schema()},
				[shared](tool_context const& call) { return run_resource(shared, "resources/list", call); });
			tools->register_tool(
				ctx,
				tool_definition{
					"list_mcp_resource_templates",
					"List parameterized resource URI templates from an MCP server.",
					list_schema()},
				[shared](tool_context const& call) { return run_resource(shared, "resources/templates/list", call); });
			tools->register_tool(
				ctx,
				tool_definition{
					"read_mcp_resource",
					"Read an MCP resource by URI from the named server. Use a listed URI or an expanded resource "
					"template.",
					read_schema()},
				[shared](tool_context const& call) { return run_resource(shared, "resources/read", call); });
			co_return;
		}
	};
	return std::make_unique<mcp_resources_plugin>();
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"mcp", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"mcp-resources", g_deps, g_provs, &make_mcp_resources, {}};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::mcp_resources
