#include "araya/agent-instructions/agent_instructions.hpp"

#include "araya/config.hpp"
#include "araya/system-prompt/system_prompt.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace araya::agent_instructions {
namespace {

namespace fs = std::filesystem;

constexpr araya::config_key<std::uint64_t> max_bytes_key{"max_bytes"};
constexpr araya::config_key<std::uint64_t> max_source_bytes_key{"max_source_bytes"};
constexpr araya::config_key<std::string> root_markers_key{"project_root_markers"};
constexpr araya::config_key<std::string> candidates_key{"instruction_file_candidates"};
constexpr araya::config_key<std::string> local_candidates_key{"local_instruction_file_candidates"};

constexpr std::string_view intro =
	"The following workspace instructions may be relevant to your work. Use them as guidance when applicable. More "
	"specific instructions take precedence over broader ones. They do not override system, developer, or direct user "
	"instructions.";

struct config {
	std::size_t max_bytes = 65536;
	std::size_t max_source_bytes = 1048576;
	std::vector<std::string> root_markers{".git"};
	std::vector<std::string> candidates{"AGENTS.md", "CLAUDE.md"};
	std::vector<std::string> local_candidates{"AGENTS.local.md", "CLAUDE.local.md"};
};

std::string trim(std::string_view text) {
	auto const first = text.find_first_not_of(" \t\r\n");
	if (first == std::string_view::npos)
		return {};
	auto const last = text.find_last_not_of(" \t\r\n");
	return std::string(text.substr(first, last - first + 1));
}

std::vector<std::string> split_csv(std::string_view text) {
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= text.size()) {
		auto const comma = text.find(',', start);
		auto const end = comma == std::string_view::npos ? text.size() : comma;
		auto item = trim(text.substr(start, end - start));
		if (!item.empty())
			out.push_back(std::move(item));
		if (comma == std::string_view::npos)
			break;
		start = comma + 1;
	}
	return out;
}

config parse_config(araya::plugin_config const& raw) {
	araya::plugin_config_view const view(raw);
	config out;
	if (auto value = view.try_get(max_bytes_key))
		out.max_bytes = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(max_source_bytes_key))
		out.max_source_bytes = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(root_markers_key)) {
		auto parsed = split_csv(*value);
		if (!parsed.empty())
			out.root_markers = std::move(parsed);
	}
	if (auto value = view.try_get(candidates_key)) {
		auto parsed = split_csv(*value);
		if (!parsed.empty())
			out.candidates = std::move(parsed);
	}
	if (auto value = view.try_get(local_candidates_key))
		out.local_candidates = split_csv(*value);
	return out;
}

bool has_marker(fs::path const& dir, std::vector<std::string> const& markers) {
	std::error_code ec;
	for (auto const& marker : markers) {
		if (fs::exists(dir / marker, ec) && !ec)
			return true;
		ec.clear();
	}
	return false;
}

// Walk upward from cwd to the first directory containing a root marker; when
// none is found the cwd itself is the root.
fs::path find_project_root(fs::path cwd, std::vector<std::string> const& markers) {
	std::error_code ec;
	fs::path current = fs::absolute(cwd, ec);
	if (ec)
		current = cwd;
	for (;;) {
		if (has_marker(current, markers))
			return current;
		auto const parent = current.parent_path();
		if (parent.empty() || parent == current)
			return fs::absolute(cwd, ec);
		current = parent;
	}
}

// The inclusive root-to-cwd chain, broadest first.
std::vector<fs::path> ancestor_chain(fs::path const& root, fs::path cwd) {
	std::vector<fs::path> chain;
	std::error_code ec;
	auto const resolved_root = fs::weakly_canonical(root, ec);
	auto current = fs::weakly_canonical(cwd, ec);
	while (current != resolved_root && current.has_parent_path() && current.parent_path() != current) {
		chain.push_back(current);
		current = current.parent_path();
	}
	chain.push_back(resolved_root);
	std::reverse(chain.begin(), chain.end());
	return chain;
}

struct discovered_file {
	fs::path absolute;
	std::string display;
	std::string version;
};

std::string version_of(fs::path const& path) {
	std::error_code ec;
	auto const mtime = fs::last_write_time(path, ec);
	if (ec)
		return {};
	auto const size = fs::file_size(path, ec);
	if (ec)
		return {};
	return std::to_string(mtime.time_since_epoch().count()) + ":" + std::to_string(size);
}

std::vector<discovered_file> discover(fs::path const& cwd, config const& cfg, fs::path const& root) {
	std::vector<discovered_file> files;
	std::set<std::string> seen;
	for (auto const& dir : ancestor_chain(root, cwd)) {
		for (auto const* candidates : {&cfg.candidates, &cfg.local_candidates}) {
			for (auto const& name : *candidates) {
				fs::path const path = dir / name;
				std::error_code ec;
				if (!fs::is_regular_file(path, ec) || ec)
					continue;
				auto absolute = path;
				auto const canonical = fs::weakly_canonical(path, ec);
				if (!ec)
					absolute = canonical;
				if (!seen.insert(absolute.generic_string()).second)
					continue;
				auto display = fs::relative(path, root, ec);
				if (ec || display.empty())
					display = path;
				files.push_back(discovered_file{absolute, display.generic_string(), version_of(path)});
			}
		}
	}
	return files;
}

std::optional<std::string> read_bounded(fs::path const& path, std::size_t max_source_bytes) {
	std::error_code ec;
	auto const size = fs::file_size(path, ec);
	if (ec || size > max_source_bytes)
		return std::nullopt;
	std::ifstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	std::ostringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

struct loaded_file {
	std::string display;
	std::string content;
};

// Drop later same-directory candidates whose trimmed content duplicates an
// earlier sibling (different directories never collapse).
std::vector<loaded_file> dedup(std::vector<loaded_file> files) {
	std::map<std::string, std::set<std::string>> seen;
	std::vector<loaded_file> kept;
	for (auto& file : files) {
		auto const dir = fs::path(file.display).parent_path().generic_string();
		auto const digest = trim(file.content);
		if (!seen[dir].insert(digest).second)
			continue;
		kept.push_back(std::move(file));
	}
	return kept;
}

std::string escape(std::string_view body) {
	constexpr std::string_view close = "</system-reminder>";
	std::string out;
	out.reserve(body.size());
	for (std::size_t at = body.find(close); at != std::string_view::npos; at = body.find(close, at)) {
		out.append(body.substr(0, at));
		out += "<\\/system-reminder>";
		body.remove_prefix(at + close.size());
	}
	out.append(body);
	return out;
}

// UTF-8-safe truncation to at most max_bytes.
std::string truncate_utf8(std::string_view text, std::size_t max_bytes) {
	if (text.size() <= max_bytes)
		return std::string(text);
	std::size_t end = max_bytes;
	while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80)
		--end;
	return std::string(text.substr(0, end));
}

std::string join(std::vector<std::string> const& items, std::string_view separator) {
	std::string out;
	for (auto const& item : items) {
		if (!out.empty())
			out += separator;
		out += item;
	}
	return out;
}

std::string wrap(std::string body) { return "<system-reminder>\n" + escape(body) + "\n</system-reminder>"; }

std::string section_text(loaded_file const& file) {
	return "Instructions from: " + file.display + "\n\n" + file.content;
}

// Render within the byte budget: include everything when it fits, else drop
// the broadest files one at a time, then truncate the most specific.
std::string render(std::vector<loaded_file> const& files, std::size_t max_bytes) {
	if (files.empty() || max_bytes == 0)
		return {};

	auto budget_marker = [&](std::vector<std::string> const& omitted, std::optional<std::string> const& truncated) {
		std::vector<std::string> parts;
		if (!omitted.empty())
			parts.push_back("omitted " + join(omitted, ", "));
		if (truncated)
			parts.push_back(*truncated);
		return "Workspace instruction budget " + std::to_string(max_bytes) + " bytes: " + join(parts, "; ");
	};
	auto build = [&](std::vector<loaded_file> const& kept, std::string const& marker) {
		std::vector<std::string> blocks;
		if (!marker.empty())
			blocks.push_back(marker);
		blocks.push_back(std::string(intro));
		for (auto const& file : kept)
			blocks.push_back(section_text(file));
		return wrap(join(blocks, "\n\n"));
	};

	auto full = build(files, {});
	if (full.size() <= max_bytes)
		return full;

	for (std::size_t start = 1; start < files.size(); ++start) {
		std::vector<loaded_file> kept(files.begin() + static_cast<std::ptrdiff_t>(start), files.end());
		std::vector<std::string> omitted;
		for (std::size_t i = 0; i < start; ++i)
			omitted.push_back(files[i].display);
		auto candidate = build(kept, budget_marker(omitted, std::nullopt));
		if (candidate.size() <= max_bytes)
			return candidate;
	}

	// Only the most specific file remains: truncate its content to fit.
	loaded_file const& last = files.back();
	std::vector<std::string> omitted;
	for (std::size_t i = 0; i + 1 < files.size(); ++i)
		omitted.push_back(files[i].display);
	auto candidate_for = [&](std::size_t content_bytes) {
		loaded_file truncated{last.display, truncate_utf8(last.content, content_bytes)};
		std::string marker = budget_marker(
			omitted,
			"truncated " + last.display + " from " + std::to_string(last.content.size()) + " to " +
				std::to_string(truncated.content.size()) + " bytes");
		std::vector<loaded_file> kept{std::move(truncated)};
		return build(kept, marker);
	};
	std::size_t low = 0;
	std::size_t high = last.content.size();
	std::size_t best = 0;
	while (low <= high) {
		auto const mid = low + (high - low) / 2;
		if (candidate_for(mid).size() <= max_bytes) {
			best = mid;
			low = mid + 1;
		} else {
			if (mid == 0)
				break;
			high = mid - 1;
		}
	}
	return candidate_for(best);
}

struct cache_entry {
	std::string signature;
	std::string text;
};

struct plugin_state {
	config cfg;
	std::map<std::string, cache_entry> cache;

	std::string instructions_for(std::string cwd) {
		if (cwd.empty()) {
			std::error_code ec;
			cwd = fs::current_path(ec).string();
		}
		fs::path const path = cwd;
		auto const root = find_project_root(path, cfg.root_markers);
		auto const files = discover(path, cfg, root);

		std::string signature = cwd;
		for (auto const& file : files)
			signature += "|" + file.display + ":" + file.version;
		auto& entry = cache[cwd];
		if (entry.signature == signature)
			return entry.text;

		std::vector<loaded_file> loaded;
		for (auto const& file : files) {
			if (auto content = read_bounded(file.absolute, cfg.max_source_bytes))
				loaded.push_back(loaded_file{file.display, std::move(*content)});
		}
		entry.text = render(dedup(std::move(loaded)), cfg.max_bytes);
		entry.signature = std::move(signature);
		return entry.text;
	}
};

struct agent_instructions_plugin : araya::plugin {
	explicit agent_instructions_plugin(araya::plugin_config config) {
		state_ = std::make_shared<plugin_state>();
		state_->cfg = parse_config(config);
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto state = state_;
		araya::system_prompt::prompt_section section;
		section.name = "agent-instructions";
		section.order = araya::system_prompt::section_order("AGENT_INSTRUCTIONS");
		section.render = [state](araya::system_prompt::assemble_context const& context) {
			return state->instructions_for(context.cwd);
		};
		prompts->section(ctx, std::move(section));
		co_return;
	}

private:
	std::shared_ptr<plugin_state> state_;
};

std::unique_ptr<araya::plugin> make_agent_instructions(araya::plugin_config const& config) {
	return std::make_unique<agent_instructions_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"system-prompt", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::plugin_descriptor g_descriptor{"agent-instructions", g_deps, g_no_provs, &make_agent_instructions};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::agent_instructions
