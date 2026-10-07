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

namespace araya::lsp_stdio {
namespace {

bool valid_id(std::string_view id) {
	auto const first = id.find_first_not_of(" \t\r\n");
	return first != std::string_view::npos;
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

server_config parse_server(std::string id, boost::json::value const& value) {
	auto const* object = value.if_object();
	if (!object)
		throw std::invalid_argument("lsp-stdio: server '" + id + "' must be an object");
	server_config server;
	server.id = std::move(id);
	if (auto const* field = object->if_contains("command"); field && field->is_string())
		server.command = expand_vars(field->as_string());
	if (server.command.empty())
		throw std::invalid_argument("lsp-stdio: server '" + server.id + "' needs a non-empty command");
	if (auto const* field = object->if_contains("args"); field && field->is_array()) {
		for (auto const& entry : field->as_array()) {
			if (!entry.is_string())
				throw std::invalid_argument("lsp-stdio: server '" + server.id + "' args must be strings");
			server.args.push_back(expand_vars(entry.as_string()));
		}
	}
	if (auto const* field = object->if_contains("extensionToLanguage"); field && field->is_object()) {
		for (auto const& [ext, language] : field->as_object()) {
			server.extension_to_language.emplace_back(std::string(ext), araya::util::json::get_string(language));
		}
	}
	if (server.extension_to_language.empty())
		throw std::invalid_argument("lsp-stdio: server '" + server.id + "' needs a non-empty extensionToLanguage");
	if (auto const* field = object->if_contains("env"))
		server.env = parse_string_map(*field);
	if (auto const* field = object->if_contains("initializationOptions"))
		server.initialization_options = *field;
	if (auto const* field = object->if_contains("configuration"))
		server.configuration = *field;
	if (auto const value = araya::util::json::opt_uint(*object, "maxMessageBytes"))
		server.max_message_bytes = static_cast<std::size_t>(*value);
	if (auto const value = araya::util::json::opt_uint(*object, "maxStderrBytes"))
		server.max_stderr_bytes = static_cast<std::size_t>(*value);
	if (auto const value = araya::util::json::opt_uint(*object, "maxDocumentBytes"))
		server.max_document_bytes = static_cast<std::size_t>(*value);
	if (auto const value = araya::util::json::opt_uint(*object, "shutdownTimeoutMs"))
		server.shutdown_timeout_ms = *value;
	if (auto const value = araya::util::json::opt_uint(*object, "killGraceMs"))
		server.kill_grace_ms = *value;
	if (auto const value = araya::util::json::opt_uint(*object, "requestTimeoutMs"))
		server.request_timeout_ms = *value;
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

lsp_stdio_config parse_config(araya::plugin_config const& config) {
	std::string const text = araya::util::read_config_text(config, "lsp-stdio");
	if (text.empty())
		return {};
	boost::json::value const root = araya::util::parse_config_json(text, "lsp-stdio");
	auto const& object = root.as_object();

	lsp_stdio_config out;
	if (auto const* servers = object.if_contains("servers"); servers) {
		auto const* map = servers->if_object();
		if (!map)
			throw std::invalid_argument("lsp-stdio: 'servers' must be an object");
		for (auto const& [id, value] : *map) {
			std::string const server_id(id);
			if (!valid_id(server_id))
				throw std::invalid_argument("lsp-stdio: server ids must be non-empty strings");
			out.servers.push_back(parse_server(server_id, value));
		}
	}
	return out;
}

} // namespace araya::lsp_stdio
