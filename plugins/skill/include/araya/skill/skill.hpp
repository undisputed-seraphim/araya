#pragma once

#include "araya/plugin.hpp"
#include "araya/service.hpp"

#include <optional>
#include <string>
#include <vector>

// The skill registry and its local filesystem provider. A skill is a reusable
// set of task-specific instructions authored on disk as a directory bundle
// `<name>/SKILL.md` or a flat `<name>.md` under a scanned root; the catalog is
// read on demand and the body is re-read at load time.
//
// A feature-replication of the deepseek-harness `@deepseek-ai/dsh-skill` +
// `@deepseek-ai/dsh-skill-filesystem`, trimmed to one filesystem provider (no
// provider registry, watcher, remote sources, or user roots).
namespace araya::skill {

// One catalog entry (no body).
struct skill_summary {
	std::string name;
	std::string description;
	std::optional<std::string> when_to_use;
	// Absolute path to the instruction file.
	std::string path;
	// The discovery source: `project-dsh`, `project-agents`, or `custom`.
	std::string source;
	// Whether the model-facing catalog and loader include this skill.
	bool model_invocable = true;
	// Whether a human-facing surface may include this skill.
	bool user_invocable = true;
};

// A fully loaded skill: its summary plus the instruction body.
struct skill_definition {
	skill_summary summary;
	std::string content;
};

class skills_service {
public:
	virtual ~skills_service() = default;

	// The merged catalog for `cwd`'s project root, sorted by name. Empty cwd
	// scans the process working directory.
	virtual std::vector<skill_summary> list(std::string const& cwd) const = 0;

	// Load one skill by exact kebab-case name, re-reading the file. Nullopt
	// for an unknown name or a skill that no longer loads.
	virtual std::optional<skill_definition> get(std::string const& name, std::string const& cwd) const = 0;
};

inline constexpr araya::service_key<skills_service> skills_key{"skills", 1};

// The plugin descriptor: provides `skills`; no dependencies.
araya::plugin_descriptor const& plugin_descriptor();

} // namespace araya::skill
