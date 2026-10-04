#pragma once

#include "araya/plugin.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

// The app-level configuration surface: a layered JSON config resolved from
// built-in defaults, the home overlay, the project overlay, explicit
// `--config` overlays, the environment, and CLI flags (see doc/CONFIG.md).
namespace araya::app {

// A non-fatal config problem (an unknown key, an unknown component id).
struct config_warning {
	std::string source;
	std::string detail;
};

// Command-line overrides; these outrank every other layer.
struct config_cli {
	std::optional<std::string> log_file;
	std::optional<std::string> log_level;
	std::optional<std::string> llm_config;
};

// The fully resolved configuration. Paths are absolute after resolution.
struct app_config {
	std::string log_file;
	std::string log_level = "info";
	std::string state_dir;
	std::string sessions_dir;
	std::string attachments_dir;
	// The LLM provider: a path (config_file) or an inline provider object.
	std::optional<std::string> llm_config_file;
	std::optional<std::string> llm_config_json;
	// Per-component knob overrides, already stringified.
	std::map<std::string, araya::plugin_config> components;
	// Per-component key provenance (source label) for --print-config.
	std::map<std::string, std::map<std::string, std::string>> component_sources;
	// Component enable switches: true drops the component.
	std::map<std::string, bool> disabled;
	// The layers that contributed, newest last (for display).
	std::vector<std::string> layers;
	// The inputs, retained so /config reload can resolve again.
	std::vector<std::string> overlays;
	config_cli overrides;
};

// The state base directory: `$ARAYA_STATE_DIR`, else `$XDG_STATE_HOME/araya`,
// else `~/.local/state/araya`, else the working directory.
std::string resolve_state_dir();

// Resolve the layered configuration. `overlays` are explicit `--config`
// paths (applied in order after the home and project layers). Throws
// std::runtime_error on an unreadable file, malformed JSON, or a misplaced
// value. Unknown top-level keys are collected into `warnings`.
app_config resolve_app_config(
	std::vector<std::string> const& overlays,
	config_cli const& cli,
	std::vector<config_warning>& warnings);

// A descriptor lookup (the app's `real_descriptor`); config.cpp does not
// depend on the app so this module stays unit-testable.
using descriptor_lookup = araya::plugin_descriptor const* (*)(std::string_view);

// Validate one component block against its descriptor's declared schema:
// a malformed known value throws config_error; an unknown key is reported.
// A null descriptor reports the unknown component.
void validate_component(
	descriptor_lookup lookup,
	std::string const& id,
	araya::plugin_config const& config,
	std::vector<config_warning>& warnings);

// Render the resolved configuration and every accepted plugin knob, for
// `--print-config`.
std::string render_app_config(app_config const& config, descriptor_lookup lookup);

} // namespace araya::app
