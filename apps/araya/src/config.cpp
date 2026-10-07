#include "config.hpp"

#include "araya/config.hpp"

#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/value.hpp>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace araya::app {
namespace {

namespace fs = std::filesystem;
namespace json = boost::json;

std::string const& cwd() {
	static std::string const value = fs::current_path().string();
	return value;
}

std::optional<std::string> env(std::string const& name) {
	if (auto const* value = std::getenv(name.c_str()); value && *value)
		return std::string(value);
	return std::nullopt;
}

std::string home_dir() { return env("HOME").value_or(""); }

// Expands `$VAR` and `${VAR}` against the environment.
std::string expand_vars(std::string const& value) {
	std::string out;
	out.reserve(value.size());
	for (std::size_t i = 0; i < value.size();) {
		if (value[i] == '$' && i + 1 < value.size()) {
			std::size_t const start = i + 1;
			if (value[start] == '{') {
				auto const close = value.find('}', start + 1);
				if (close != std::string::npos) {
					if (auto resolved = env(value.substr(start + 1, close - start - 1)))
						out += *resolved;
					i = close + 1;
					continue;
				}
			} else {
				std::size_t end = start;
				while (end < value.size() &&
					   (std::isalnum(static_cast<unsigned char>(value[end])) || value[end] == '_'))
					++end;
				if (end > start) {
					if (auto resolved = env(value.substr(start, end - start)))
						out += *resolved;
					i = end;
					continue;
				}
			}
		}
		out.push_back(value[i++]);
	}
	return out;
}

// Expands `~` and `$VAR`; a relative result resolves against `base`.
std::string expand_path(std::string const& value, std::string const& base) {
	std::string v = expand_vars(value);
	if (v.empty())
		return v;
	if (v == "~")
		v = home_dir();
	else if (v.rfind("~/", 0) == 0)
		v = home_dir() + v.substr(1);
	if (fs::path(v).is_absolute())
		return v;
	return (fs::path(base) / v).string();
}

std::string xdg_config_home() {
	if (auto value = env("XDG_CONFIG_HOME"))
		return *value;
	return (fs::path(home_dir()) / ".config").string();
}

std::optional<std::string> read_file(std::string const& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return std::nullopt;
	std::ostringstream buffer;
	buffer << in.rdbuf();
	return buffer.str();
}

// JSON scalars become their string form; arrays/objects serialize compactly.
std::string stringify(json::value const& value) {
	if (value.is_string())
		return std::string(value.as_string());
	return json::serialize(value);
}

void require_object(json::value const& value, std::string const& source, std::string_view key) {
	if (!value.is_object())
		throw std::runtime_error(source + ": '" + std::string(key) + "' must be a JSON object");
}

} // namespace

std::string resolve_state_dir() {
	if (auto dir = env("ARAYA_STATE_DIR"))
		return expand_path(*dir, cwd());
	if (auto state = env("XDG_STATE_HOME"))
		return (fs::path(*state) / "araya").string();
	if (!home_dir().empty())
		return (fs::path(home_dir()) / ".local" / "state" / "araya").string();
	return cwd();
}

namespace {

void apply_layer(
	app_config& cfg,
	json::object const& object,
	std::string const& source,
	std::vector<config_warning>& warnings) {
	for (auto const& [key, value] : object) {
		if (key == "log") {
			require_object(value, source, key);
			for (auto const& [name, field] : value.as_object()) {
				if (name == "file" && field.is_string())
					cfg.log_file = expand_path(std::string(field.as_string()), cfg.state_dir);
				else if (name == "level" && field.is_string())
					cfg.log_level = std::string(field.as_string());
				else
					warnings.push_back({source, "unknown log key '" + std::string(name) + "'"});
			}
		} else if (key == "state") {
			require_object(value, source, key);
			for (auto const& [name, field] : value.as_object()) {
				if (name == "sessions" && field.is_string())
					cfg.sessions_dir = expand_path(std::string(field.as_string()), cfg.state_dir);
				else if (name == "attachments" && field.is_string())
					cfg.attachments_dir = expand_path(std::string(field.as_string()), cfg.state_dir);
				else
					warnings.push_back({source, "unknown state key '" + std::string(name) + "'"});
			}
		} else if (key == "llm") {
			require_object(value, source, key);
			for (auto const& [name, field] : value.as_object()) {
				if (name == "config_file" && field.is_string()) {
					cfg.llm_config_file = expand_path(std::string(field.as_string()), cfg.state_dir);
					cfg.llm_config_json.reset();
				} else if (name == "openai" && field.is_object()) {
					cfg.llm_config_json = json::serialize(field);
					cfg.llm_config_file.reset();
				} else {
					warnings.push_back({source, "unknown llm key '" + std::string(name) + "'"});
				}
			}
		} else if (key == "mcp") {
			require_object(value, source, key);
			cfg.mcp_config_json = json::serialize(value);
		} else if (key == "disabled") {
			require_object(value, source, key);
			for (auto const& [id, flag] : value.as_object()) {
				if (flag.is_bool())
					cfg.disabled[std::string(id)] = flag.as_bool();
				else
					warnings.push_back({source, "disabled.'" + std::string(id) + "' must be a boolean"});
			}
		} else if (key == "components") {
			require_object(value, source, key);
			for (auto const& [id, block] : value.as_object()) {
				if (!block.is_object()) {
					warnings.push_back({source, "components.'" + std::string(id) + "' must be an object"});
					continue;
				}
				for (auto const& [knob, field] : block.as_object()) {
					cfg.components[std::string(id)][std::string(knob)] = stringify(field);
					cfg.component_sources[std::string(id)][std::string(knob)] = source;
				}
			}
		} else {
			warnings.push_back({source, "unknown top-level key '" + std::string(key) + "'"});
		}
	}
}

void apply_file(app_config& cfg, std::string const& path, std::vector<config_warning>& warnings) {
	auto text = read_file(path);
	if (!text)
		throw std::runtime_error("araya: cannot read config file '" + path + "'");
	json::value parsed;
	try {
		parsed = json::parse(*text);
	} catch (std::exception const& e) {
		throw std::runtime_error("araya: malformed config JSON in '" + path + "': " + e.what());
	}
	if (!parsed.is_object())
		throw std::runtime_error("araya: config file '" + path + "' must hold a JSON object");
	apply_layer(cfg, parsed.as_object(), path, warnings);
	cfg.layers.push_back(path);
}

void apply_env(app_config& cfg) {
	if (auto value = env("ARAYA_LOG_FILE"))
		cfg.log_file = expand_path(*value, cwd());
	if (auto value = env("ARAYA_LOG_LEVEL"))
		cfg.log_level = *value;
	if (auto value = env("ARAYA_LLM_CONFIG")) {
		cfg.llm_config_file = expand_path(*value, cwd());
		cfg.llm_config_json.reset();
	}
	if (auto value = env("ARAYA_SYSTEM_PROMPT")) {
		cfg.components["system-prompt"]["persona_prefix"] = *value;
		cfg.component_sources["system-prompt"]["persona_prefix"] = "(env)";
	}
}

void apply_cli(app_config& cfg, config_cli const& cli) {
	if (cli.log_file)
		cfg.log_file = expand_path(*cli.log_file, cwd());
	if (cli.log_level)
		cfg.log_level = *cli.log_level;
	if (cli.llm_config) {
		cfg.llm_config_file = expand_path(*cli.llm_config, cwd());
		cfg.llm_config_json.reset();
	}
}

} // namespace

app_config resolve_app_config(
	std::vector<std::string> const& overlays,
	config_cli const& cli,
	std::vector<config_warning>& warnings) {
	app_config cfg;
	cfg.state_dir = resolve_state_dir();
	cfg.sessions_dir = (fs::path(cfg.state_dir) / "sessions").string();
	cfg.attachments_dir = (fs::path(cfg.state_dir) / "attachments").string();
	cfg.log_file = (fs::path(cfg.state_dir) / "logs" / "araya.log").string();

	auto const home = fs::path(xdg_config_home()) / "araya" / "config.json";
	if (fs::exists(home))
		apply_file(cfg, home.string(), warnings);

	auto const project = fs::path(cwd()) / "araya.json";
	if (fs::exists(project))
		apply_file(cfg, project.string(), warnings);

	std::vector<std::string> extra = overlays;
	if (auto value = env("ARAYA_CONFIG"))
		extra.push_back(*value);
	for (auto const& path : extra) {
		if (!fs::exists(path))
			throw std::runtime_error("araya: config file '" + path + "' not found");
		apply_file(cfg, path, warnings);
	}

	apply_env(cfg);
	apply_cli(cfg, cli);
	cfg.overlays = overlays;
	cfg.overrides = cli;
	return cfg;
}

void validate_component(
	descriptor_lookup lookup,
	std::string const& id,
	araya::plugin_config const& config,
	std::vector<config_warning>& warnings) {
	auto const* descriptor = lookup(id);
	if (!descriptor) {
		warnings.push_back({id, "unknown component id"});
		return;
	}
	for (auto const& [key, value] : config) {
		araya::config_field const* declared = nullptr;
		for (auto const& field : descriptor->config_schema)
			if (field.name == key) {
				declared = &field;
				break;
			}
		if (!declared) {
			warnings.push_back({id, "unknown key '" + key + "'"});
			continue;
		}
		if (declared->validate && !declared->validate(value))
			throw araya::config_error(key, value, "malformed");
	}
	for (auto const& field : descriptor->config_schema)
		if (field.required && !config.contains(std::string(field.name)))
			throw std::runtime_error(id + ": required config key '" + std::string(field.name) + "' is missing");
}

std::string render_app_config(app_config const& config, descriptor_lookup lookup) {
	std::ostringstream out;
	out << "araya configuration\n";
	out << "  log file:     " << config.log_file << "\n";
	out << "  log level:    " << config.log_level << "\n";
	out << "  state dir:    " << config.state_dir << "\n";
	out << "  sessions:     " << config.sessions_dir << "\n";
	out << "  attachments:  " << config.attachments_dir << "\n";
	if (config.llm_config_file)
		out << "  llm:          config_file " << *config.llm_config_file << "\n";
	else if (config.llm_config_json)
		out << "  llm:          inline provider object\n";
	else
		out << "  llm:          (none)\n";
	if (config.mcp_config_json)
		out << "  mcp:          configured\n";
	out << "  layers:";
	if (config.layers.empty())
		out << " (defaults only)";
	for (auto const& layer : config.layers)
		out << "\n    " << layer;
	out << "\n\ncomponents:\n";
	if (config.components.empty())
		out << "  (none configured)\n";
	for (auto const& [id, knobs] : config.components) {
		out << "  " << id << ":\n";
		auto const* descriptor = lookup(id);
		if (knobs.empty())
			out << "    (no keys)\n";
		for (auto const& [key, value] : knobs) {
			out << "    " << key << " = " << value;
			auto const sources = config.component_sources.find(id);
			if (sources != config.component_sources.end()) {
				auto const source = sources->second.find(key);
				if (source != sources->second.end())
					out << "   [" << source->second << "]";
			}
			out << "\n";
		}
		if (descriptor && !descriptor->config_schema.empty()) {
			out << "    accepted:";
			for (auto const& field : descriptor->config_schema) {
				out << " " << field.name << "<" << field.type << ">";
				if (!field.default_value.empty())
					out << "=" << field.default_value;
				if (field.required)
					out << "!";
			}
			out << "\n";
		}
	}
	out << "\ndisabled:\n";
	bool any_disabled = false;
	for (auto const& [id, flag] : config.disabled)
		if (flag) {
			out << "  " << id << "\n";
			any_disabled = true;
		}
	if (!any_disabled)
		out << "  (none)\n";
	return out.str();
}

} // namespace araya::app
