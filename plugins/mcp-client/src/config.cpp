#include "config.hpp"

#include "araya/util/json.hpp"
#include "araya/util/plugin_config_json.hpp"

#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <cctype>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::mcp {
namespace {

bool valid_server_name(std::string_view name) {
	if (name.empty() || name.size() > 32)
		return false;
	for (char const c : name) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')
			continue;
		return false;
	}
	return true;
}

reconnect_config parse_reconnect(boost::json::value const& value, reconnect_config fallback) {
	auto const* object = value.if_object();
	if (!object)
		return fallback;
	if (auto const* field = object->if_contains("enabled"); field && field->is_bool())
		fallback.enabled = field->as_bool();
	if (auto const v = araya::util::json::opt_uint(*object, "initialDelayMs"))
		fallback.initial_delay_ms = *v;
	if (auto const v = araya::util::json::opt_uint(*object, "maxDelayMs"))
		fallback.max_delay_ms = *v;
	if (auto const v = araya::util::json::opt_uint(*object, "maxAttempts"))
		fallback.max_attempts = *v;
	return fallback;
}

std::vector<std::pair<std::string, std::string>> parse_string_map(boost::json::value const& value) {
	std::vector<std::pair<std::string, std::string>> out;
	auto const* object = value.if_object();
	if (!object)
		return out;
	for (auto const& [key, entry] : *object) {
		if (entry.is_string())
			out.emplace_back(std::string(key), expand_vars(entry.as_string()));
	}
	return out;
}

server_config parse_server(std::string name, boost::json::value const& value, mcp_config const& defaults) {
	auto const* object = value.if_object();
	if (!object)
		throw std::invalid_argument("mcp: server '" + name + "' must be an object");
	server_config server;
	server.name = std::move(name);
	server.timeout_ms = defaults.request_timeout_ms;
	server.max_instruction_bytes = defaults.max_instruction_bytes;
	server.fail_on_startup_error = defaults.fail_on_startup_error;
	server.reconnect = defaults.reconnect;

	if (auto const* field = object->if_contains("enabled"); field && field->is_bool())
		server.enabled = field->as_bool();
	if (auto const* field = object->if_contains("type"); field && field->is_string())
		server.transport = std::string(field->as_string());
	if (auto const v = araya::util::json::opt_uint(*object, "timeoutMs"))
		server.timeout_ms = *v;
	if (auto const v = araya::util::json::opt_uint(*object, "maxInstructionBytes"))
		server.max_instruction_bytes = *v;
	if (auto const* field = object->if_contains("failOnStartupError"); field && field->is_bool())
		server.fail_on_startup_error = field->as_bool();
	if (auto const* field = object->if_contains("reconnect"))
		server.reconnect = parse_reconnect(*field, server.reconnect);

	if (auto const* field = object->if_contains("cwd"); field && field->is_string())
		server.cwd = expand_vars(field->as_string());
	if (auto const* field = object->if_contains("url"); field && field->is_string())
		server.url = expand_vars(field->as_string());
	if (auto const* field = object->if_contains("environment"))
		server.environment = parse_string_map(*field);
	if (auto const* field = object->if_contains("headers"))
		server.headers = parse_string_map(*field);
	if (auto const* field = object->if_contains("command"); field && field->is_array()) {
		for (auto const& entry : field->as_array()) {
			if (!entry.is_string())
				throw std::invalid_argument("mcp: server '" + server.name + "' command entries must be strings");
			server.command.push_back(expand_vars(entry.as_string()));
		}
	}

	if (server.transport == "local") {
		if (server.command.empty())
			throw std::invalid_argument("mcp: local server '" + server.name + "' needs a non-empty command");
	} else if (server.transport == "remote") {
		if (server.url.empty())
			throw std::invalid_argument("mcp: remote server '" + server.name + "' needs a url");
	} else {
		throw std::invalid_argument("mcp: server '" + server.name + "' has unknown type '" + server.transport + "'");
	}
	return server;
}

} // namespace

std::string expand_vars(std::string_view text) {
	std::string out;
	out.reserve(text.size());
	for (std::size_t i = 0; i < text.size();) {
		if (text[i] == '$' && i + 1 < text.size() && text[i + 1] == '{') {
			auto const close = text.find('}', i + 2);
			if (close != std::string_view::npos) {
				std::string const name(text.substr(i + 2, close - (i + 2)));
				if (char const* value = std::getenv(name.c_str()))
					out += value;
				i = close + 1;
				continue;
			}
		}
		out.push_back(text[i]);
		++i;
	}
	return out;
}

bool sensitive_env_name(std::string_view name) {
	if (name.rfind("ARAYA_", 0) == 0)
		return true;
	std::string lowered;
	lowered.reserve(name.size());
	for (char const c : name)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	for (std::string_view needle : {"key", "password", "secret", "token"}) {
		if (lowered.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

mcp_config parse_config(araya::plugin_config const& config) {
	std::string const text = araya::util::read_config_text(config, "mcp-client");
	if (text.empty())
		return {};
	boost::json::value const root = araya::util::parse_config_json(text, "mcp-client");
	auto const& object = root.as_object();

	mcp_config out;
	if (auto const v = araya::util::json::opt_uint(object, "requestTimeoutMs"))
		out.request_timeout_ms = *v;
	if (auto const v = araya::util::json::opt_uint(object, "maxInstructionBytes"))
		out.max_instruction_bytes = *v;
	if (auto const* field = object.if_contains("failOnStartupError"); field && field->is_bool())
		out.fail_on_startup_error = field->as_bool();
	if (auto const* field = object.if_contains("reconnect"))
		out.reconnect = parse_reconnect(*field, out.reconnect);

	if (auto const* servers = object.if_contains("servers"); servers) {
		auto const* map = servers->if_object();
		if (!map)
			throw std::invalid_argument("mcp-client: 'servers' must be an object");
		for (auto const& [name, value] : *map) {
			std::string const server_name(name);
			if (!valid_server_name(server_name))
				throw std::invalid_argument(
					"mcp-client: server name '" + server_name + "' must match [A-Za-z0-9_-]{1,32}");
			out.servers.push_back(parse_server(server_name, value, out));
		}
	}
	return out;
}

} // namespace araya::mcp
