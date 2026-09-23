#pragma once

#include "araya/plugin.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

// The config_file-or-inline-"config" convention for plugins whose
// configuration is a JSON document (llm-openai today, more adapters
// later): config_file wins, then the inline config, then empty.
namespace araya::util {

inline std::string read_config_text(araya::plugin_config const& config, std::string_view plugin) {
	if (auto const found = config.find("config_file"); found != config.end()) {
		std::ifstream file(found->second);
		if (!file)
			throw std::invalid_argument(std::string(plugin) + ": cannot open config_file '" + found->second + "'");
		std::ostringstream contents;
		contents << file.rdbuf();
		return contents.str();
	}
	if (auto const found = config.find("config"); found != config.end())
		return found->second;
	return {};
}

inline boost::json::value parse_config_json(std::string_view text, std::string_view plugin) {
	boost::json::value json;
	try {
		json = boost::json::parse(text);
	} catch (std::exception const& e) {
		throw std::invalid_argument(std::string(plugin) + ": malformed config JSON: " + e.what());
	}
	if (!json.is_object())
		throw std::invalid_argument(std::string(plugin) + ": config JSON must be an object");
	return json;
}

} // namespace araya::util
