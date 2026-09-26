#include "araya/skill/skill.hpp"

#include "araya/config.hpp"
#include "araya/plugin_context.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::skill {
namespace {

namespace fs = std::filesystem;

constexpr araya::config_key<std::string> custom_dirs_key{"custom_skill_dirs"};

// Ranks decide duplicate-name winners (lower wins) and scan order. The
// harness's project rows plus an optional custom row.
constexpr int project_dsh_rank = 100;
constexpr int project_agents_rank = 200;
constexpr int custom_rank = 300;

struct skill_root {
	fs::path path;
	std::string source;
	int rank = 0;
};

std::string trim(std::string_view text) {
	auto const first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto const last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

std::string strip_quotes(std::string value) {
	if (value.size() >= 2 &&
		((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
		return value.substr(1, value.size() - 2);
	return value;
}

bool is_skill_name(std::string_view name) {
	if (name.empty())
		return false;
	bool previous_dash = true; // forbid a leading dash
	for (char const c : name) {
		if (c == '-') {
			if (previous_dash)
				return false;
			previous_dash = true;
		} else if (std::islower(static_cast<unsigned char>(c)) || std::isdigit(static_cast<unsigned char>(c))) {
			previous_dash = false;
		} else {
			return false;
		}
	}
	return !previous_dash;
}

// Parse one boolean frontmatter value: YAML booleans plus the case-insensitive
// truthy/falsy word forms. A rejected spelling is nullopt (the skill drops).
std::optional<bool> parse_bool(std::string value) {
	std::string lowered;
	lowered.reserve(value.size());
	for (char const c : value)
		lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
	if (lowered == "true" || lowered == "yes" || lowered == "on" || lowered == "1")
		return true;
	if (lowered == "false" || lowered == "no" || lowered == "off" || lowered == "0")
		return false;
	return std::nullopt;
}

struct parsed_skill {
	std::string name;
	std::string description;
	std::optional<std::string> when_to_use;
	bool model_invocable = true;
	bool user_invocable = true;
	std::string content;
};

// Parse a skill file: YAML frontmatter must open on the first line with `---`
// and close on a later `---` line; the body follows. Required `name` and
// `description`. Unknown keys are ignored (the harness parses full YAML; we
// read the flat key/value subset these files use).
std::optional<parsed_skill> parse_skill_file(fs::path const& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	std::stringstream buffer;
	buffer << stream.rdbuf();
	std::string const text = buffer.str();

	auto const first_end = text.find('\n');
	if (first_end == std::string::npos || trim(std::string_view(text).substr(0, first_end)) != "---")
		return std::nullopt;

	std::size_t cursor = first_end + 1;
	std::optional<std::size_t> body_start;
	std::string name;
	std::string description;
	std::optional<std::string> when_to_use;
	bool model_invocable = true;
	bool user_invocable = true;

	while (cursor <= text.size()) {
		auto const line_end = text.find('\n', cursor);
		auto line = std::string_view(text).substr(
			cursor, line_end == std::string::npos ? std::string_view::npos : line_end - cursor);
		auto const stripped = trim(line);
		if (stripped == "---") {
			body_start = line_end == std::string::npos ? text.size() : line_end + 1;
			break;
		}
		auto const colon = stripped.find(':');
		if (colon != std::string::npos) {
			auto const key = trim(std::string_view(stripped).substr(0, colon));
			auto const value = strip_quotes(trim(std::string_view(stripped).substr(colon + 1)));
			if (key == "name") {
				name = value;
			} else if (key == "description") {
				description = value;
			} else if (key == "whenToUse") {
				if (!value.empty())
					when_to_use = value;
			} else if (key == "disable-model-invocation") {
				auto parsed = parse_bool(value);
				if (!parsed)
					return std::nullopt;
				model_invocable = !*parsed;
			} else if (key == "user-invocable") {
				auto parsed = parse_bool(value);
				if (!parsed)
					return std::nullopt;
				user_invocable = *parsed;
			}
		}
		if (line_end == std::string::npos)
			break;
		cursor = line_end + 1;
	}
	if (!body_start)
		return std::nullopt;
	if (!is_skill_name(name) || description.empty())
		return std::nullopt;

	parsed_skill parsed;
	parsed.name = std::move(name);
	parsed.description = std::move(description);
	parsed.when_to_use = std::move(when_to_use);
	parsed.model_invocable = model_invocable;
	parsed.user_invocable = user_invocable;
	parsed.content = trim(std::string_view(text).substr(*body_start));
	return parsed;
}

// The nearest ancestor containing `.git`, or the resolved cwd when none.
fs::path project_root(fs::path const& cwd) {
	std::error_code ec;
	auto current = fs::absolute(cwd, ec);
	if (ec)
		current = cwd;
	for (auto probe = current; !probe.empty(); probe = probe.parent_path()) {
		if (fs::exists(probe / ".git", ec))
			return probe;
		if (probe.parent_path() == probe)
			break;
	}
	return current;
}

std::vector<skill_root> roots_for(fs::path const& cwd, std::vector<std::string> const& custom_dirs) {
	auto const root = project_root(cwd);
	std::vector<skill_root> roots{
		{root / ".dsh" / "skills", "project-dsh", project_dsh_rank},
		{root / ".agents" / "skills", "project-agents", project_agents_rank},
	};
	for (auto const& dir : custom_dirs) {
		if (!dir.empty())
			roots.push_back({fs::path(dir), "custom", custom_rank});
	}
	return roots;
}

struct candidate {
	skill_summary summary;
	int rank = 0;
};

// Discover the top-level skills of one root: directory bundles
// (`<name>/SKILL.md`) and flat `<name>.md` files. Nested SKILL.md files are
// deliberately not discovered.
std::vector<candidate> discover_root(skill_root const& root) {
	std::vector<candidate> found;
	std::error_code ec;
	if (!fs::is_directory(root.path, ec))
		return found;
	for (auto const& entry : fs::directory_iterator(root.path, fs::directory_options::skip_permission_denied, ec)) {
		if (ec)
			break;
		fs::path file;
		if (entry.is_directory(ec)) {
			file = entry.path() / "SKILL.md";
			if (!fs::is_regular_file(file, ec))
				continue;
		} else if (entry.is_regular_file(ec) && entry.path().extension() == ".md") {
			file = entry.path();
		} else {
			continue;
		}
		auto parsed = parse_skill_file(file);
		if (!parsed)
			continue;
		candidate entry_candidate;
		entry_candidate.rank = root.rank;
		entry_candidate.summary.name = std::move(parsed->name);
		entry_candidate.summary.description = std::move(parsed->description);
		entry_candidate.summary.when_to_use = std::move(parsed->when_to_use);
		entry_candidate.summary.path = fs::absolute(file, ec).string();
		entry_candidate.summary.source = root.source;
		entry_candidate.summary.model_invocable = parsed->model_invocable;
		entry_candidate.summary.user_invocable = parsed->user_invocable;
		found.push_back(std::move(entry_candidate));
	}
	return found;
}

class filesystem_skills : public skills_service {
public:
	explicit filesystem_skills(std::vector<std::string> custom_dirs)
		: custom_dirs_(std::move(custom_dirs)) {}

	std::vector<skill_summary> list(std::string const& cwd) const override {
		auto const base = cwd.empty() ? fs::current_path() : fs::path(cwd);
		auto roots = roots_for(base, custom_dirs_);
		std::stable_sort(
			roots.begin(), roots.end(), [](skill_root const& a, skill_root const& b) { return a.rank < b.rank; });

		std::vector<candidate> candidates;
		for (auto const& root : roots) {
			auto found = discover_root(root);
			candidates.insert(
				candidates.end(), std::make_move_iterator(found.begin()), std::make_move_iterator(found.end()));
		}
		std::stable_sort(candidates.begin(), candidates.end(), [](candidate const& a, candidate const& b) {
			if (a.rank != b.rank)
				return a.rank < b.rank;
			return a.summary.name < b.summary.name;
		});

		std::vector<skill_summary> out;
		std::vector<std::string> seen;
		for (auto& candidate : candidates) {
			if (std::find(seen.begin(), seen.end(), candidate.summary.name) != seen.end())
				continue;
			seen.push_back(candidate.summary.name);
			out.push_back(std::move(candidate.summary));
		}
		std::sort(
			out.begin(), out.end(), [](skill_summary const& a, skill_summary const& b) { return a.name < b.name; });
		return out;
	}

	std::optional<skill_definition> get(std::string const& name, std::string const& cwd) const override {
		if (!is_skill_name(name))
			return std::nullopt;
		for (auto const& summary : list(cwd)) {
			if (summary.name != name)
				continue;
			auto parsed = parse_skill_file(fs::path(summary.path));
			if (!parsed || parsed->name != name)
				return std::nullopt;
			skill_definition definition;
			definition.summary = summary;
			definition.content = std::move(parsed->content);
			return definition;
		}
		return std::nullopt;
	}

private:
	std::vector<std::string> custom_dirs_;
};

std::unique_ptr<araya::plugin> make_skill(araya::plugin_config const& config) {
	struct skill_plugin : araya::plugin {
		explicit skill_plugin(araya::plugin_config const& cfg) {
			araya::plugin_config_view const view(cfg);
			if (auto value = view.try_get(custom_dirs_key)) {
				std::string current;
				for (char const c : *value) {
					if (c == ':') {
						custom_dirs.push_back(current);
						current.clear();
					} else {
						current.push_back(c);
					}
				}
				if (!current.empty())
					custom_dirs.push_back(current);
			}
		}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			std::shared_ptr<skills_service> impl = std::make_shared<filesystem_skills>(custom_dirs);
			ctx.provide(skills_key, std::move(impl));
			co_return;
		}

		std::vector<std::string> custom_dirs;
	};
	return std::make_unique<skill_plugin>(config);
}

static constexpr std::span<araya::dependency_spec const> g_deps{};
static const araya::provision_spec g_provs[]{{araya::service_id{"skills", 1}}};
static const araya::plugin_descriptor g_descriptor{"skill", g_deps, g_provs, &make_skill};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::skill
