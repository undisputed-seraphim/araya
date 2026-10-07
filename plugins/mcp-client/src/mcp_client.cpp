#include "araya/mcp-client/mcp.hpp"

#include "araya/config.hpp"
#include "araya/plugin_context.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include "config.hpp"
#include "connection.hpp"
#include "naming.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::mcp {
namespace {

using araya::plugin_config;
using araya::plugin_context;
using araya::registration;
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

constexpr araya::config_key<std::string> config_file_key{"config_file"};
constexpr araya::config_key<std::string> config_inline_key{"config"};

constexpr araya::config_field g_config[] = {
	field(config_file_key, "Path to the MCP servers JSON config file."),
	field(config_inline_key, "Inline MCP servers JSON config."),
};

// Maps an MCP tools/call result onto the session content-block vocabulary.
// Text passes through; rich blocks the trimmed bridge cannot carry become
// bounded text diagnostics (never dropped silently).
tool_result render_result(boost::json::value const& result) {
	boost::json::array content;
	bool is_error = false;
	if (auto const* object = result.if_object()) {
		if (auto const* error = object->if_contains("isError"); error && error->is_bool())
			is_error = error->as_bool();
		if (auto const* blocks = object->if_contains("content"); blocks && blocks->is_array()) {
			for (auto const& block : blocks->as_array()) {
				auto const* entry = block.if_object();
				std::string const type = entry ? araya::util::json::get_string(*entry, "type") : std::string{};
				if (type == "text" && entry) {
					content.push_back(
						boost::json::object{{"type", "text"}, {"text", araya::util::json::get_string(*entry, "text")}});
				} else if (type == "image") {
					content.push_back(boost::json::object{
						{"type", "text"}, {"text", "[MCP image content is not displayed in this build]"}});
				} else if (type == "resource") {
					content.push_back(
						boost::json::object{{"type", "text"}, {"text", "[MCP embedded resource omitted]"}});
				} else if (type == "audio") {
					content.push_back(boost::json::object{{"type", "text"}, {"text", "[MCP audio content omitted]"}});
				} else if (entry) {
					content.push_back(boost::json::object{{"type", "text"}, {"text", boost::json::serialize(block)}});
				}
			}
		}
	}
	if (content.empty())
		content.push_back(boost::json::object{{"type", "text"}, {"text", boost::json::serialize(result)}});
	return tool_result{std::move(content), is_error};
}

araya::task<tool_result>
call_tool(std::weak_ptr<server_connection> weak, std::string raw_name, tool_context const& call) {
	auto connection = weak.lock();
	if (!connection)
		co_return error_result("Error: MCP server is unavailable");
	boost::json::object params;
	params["name"] = std::move(raw_name);
	params["arguments"] = call.arguments;
	try {
		boost::json::value const result = co_await connection->request("tools/call", std::move(params), call.stop);
		co_return render_result(result);
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}
}

// The plugin-owned per-server state: the activation (for late registrations),
// the registries, and the live registrations to swap on re-sync.
struct server_state {
	std::shared_ptr<araya::activation> activation;
	std::shared_ptr<tools_service> tools;
	std::shared_ptr<system_prompt_service> prompts;
	std::shared_ptr<server_connection> connection;
	std::vector<registration> tool_regs;
	registration section;
};

// Replaces one server's tool registrations with a fresh generation. Runs on
// the owning strand; a registration conflict rolls the whole generation back.
void apply_tools(server_state& state, std::string const& server, std::vector<mcp_tool> const& tools) {
	for (auto& registration_entry : state.tool_regs)
		registration_entry.release();
	state.tool_regs.clear();
	if (tools.empty())
		return;

	plugin_context caller{state.activation};
	std::vector<registration> pending;
	pending.reserve(tools.size());
	try {
		for (auto const& tool : tools) {
			std::string const name = public_tool_name(server, tool.name);
			auto connection = std::weak_ptr<server_connection>(state.connection);
			std::string raw = tool.name;
			pending.push_back(state.tools->register_tool(
				caller,
				tool_definition{name, tool.description, tool.input_schema},
				[connection, raw = std::move(raw)](tool_context const& call) {
					return call_tool(connection, raw, call);
				}));
		}
	} catch (...) {
		for (auto& registration_entry : pending)
			registration_entry.release();
		throw;
	}
	state.tool_regs = std::move(pending);
}

// Registers the per-server instructions section once. The render reads the
// connection's live instructions, so a reconnect updates the text without a
// re-registration (empty sections disappear).
void apply_section(server_state& state, std::string const& server) {
	plugin_context caller{state.activation};
	prompt_section section;
	section.name = "mcp:" + server;
	section.order = araya::system_prompt::section_order("MCP_SERVERS");
	section.interpolate = false;
	auto connection = std::weak_ptr<server_connection>(state.connection);
	section.render = [connection](assemble_context const&) {
		auto live = connection.lock();
		return live ? live->instructions() : std::string{};
	};
	state.section = state.prompts->section(caller, std::move(section));
}

class mcp_service_impl : public mcp_service {
public:
	void add(std::string name, std::shared_ptr<server_connection> connection) {
		connections_[std::move(name)] = std::move(connection);
	}

	std::vector<std::string> servers() const override {
		std::vector<std::string> names;
		names.reserve(connections_.size());
		for (auto const& [name, connection] : connections_)
			names.push_back(name);
		return names;
	}

	araya::task<boost::json::value>
	request(std::string const& server, std::string_view method, boost::json::value params, std::stop_token stop)
		override {
		auto it = connections_.find(server);
		if (it == connections_.end())
			throw std::runtime_error("mcp: unknown server '" + server + "'");
		co_return co_await it->second->request(method, std::move(params), stop);
	}

private:
	std::map<std::string, std::shared_ptr<server_connection>> connections_;
};

std::unique_ptr<araya::plugin> make_mcp_client(plugin_config const& config) {
	struct mcp_client_plugin : araya::plugin {
		explicit mcp_client_plugin(plugin_config const& cfg)
			: config(cfg) {}

		araya::task<void> apply(plugin_context& ctx) override {
			auto tools = ctx.require<tools_service>(tools_key).shared();
			auto prompts = ctx.require<system_prompt_service>(system_prompt_key).shared();
			auto parsed = parse_config(config);
			auto service = std::make_shared<mcp_service_impl>();
			ctx.provide(mcp_key, std::shared_ptr<mcp_service>{service});

			for (auto const& server : parsed.servers) {
				if (!server.enabled)
					continue;
				auto state = std::make_shared<server_state>();
				state->activation = ctx.activation_ptr();
				state->tools = tools;
				state->prompts = prompts;

				std::string const name = server.name;
				connection_observer observer;
				observer.on_tools = [state, name](std::vector<mcp_tool> const& discovered) {
					apply_tools(*state, name, discovered);
				};
				state->connection = std::make_shared<server_connection>(ctx.executor(), server, std::move(observer));
				service->add(name, state->connection);
				apply_section(*state, name);

				// Stop reconnecting and close the transport at teardown.
				ctx.effect([state]() -> araya::cleanup_action {
					return [state] {
						if (state->connection)
							state->connection->stop();
					};
				});

				state->connection->start();
				std::exception_ptr const outcome = co_await state->connection->ready();
				if (outcome && server.fail_on_startup_error)
					std::rethrow_exception(outcome);
			}
			co_return;
		}

		plugin_config config;
	};
	return std::make_unique<mcp_client_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"tools", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"mcp", 1}}};
static const araya::plugin_descriptor g_descriptor{"mcp-client", g_deps, g_provs, &make_mcp_client, g_config};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::mcp
