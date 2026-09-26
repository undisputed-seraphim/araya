#include "araya/coreutil/coreutil.hpp"

#include "araya/config.hpp"
#include "araya/fs/events.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace araya::coreutil {
namespace {

namespace fs = std::filesystem;

using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;

// -- config ----------------------------------------------------------------

constexpr araya::config_key<std::uint64_t> read_limit_key{"read_limit"};
constexpr araya::config_key<std::uint64_t> read_max_line_key{"read_max_line_length"};
constexpr araya::config_key<std::uint64_t> glob_max_key{"glob_max_results"};
constexpr araya::config_key<std::uint64_t> grep_max_key{"grep_max_matches"};
constexpr araya::config_key<std::uint64_t> grep_max_line_key{"grep_max_line_bytes"};
constexpr araya::config_key<std::uint64_t> spill_bytes_key{"spill_threshold_bytes"};
constexpr araya::config_key<std::string> disabled_key{"disabled"};

struct coreutil_config {
	std::size_t read_limit = 2000;
	std::size_t read_max_line = 2000;
	std::size_t glob_max = 100;
	std::size_t grep_max = 250;
	std::size_t grep_max_line = 2000;
	// Complete search output above this many bytes spills to a file.
	std::size_t spill_bytes = 200'000;
	// Comma-separated tool names to leave unregistered.
	std::vector<std::string> disabled;
};

coreutil_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view view(config);
	coreutil_config out;
	if (auto value = view.try_get(read_limit_key))
		out.read_limit = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(read_max_line_key))
		out.read_max_line = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(glob_max_key))
		out.glob_max = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(grep_max_key))
		out.grep_max = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(grep_max_line_key))
		out.grep_max_line = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(spill_bytes_key))
		out.spill_bytes = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(disabled_key)) {
		std::stringstream stream(*value);
		std::string item;
		while (std::getline(stream, item, ',')) {
			auto first = item.find_first_not_of(" \t");
			auto last = item.find_last_not_of(" \t");
			if (first != std::string::npos)
				out.disabled.push_back(item.substr(first, last - first + 1));
		}
	}
	return out;
}

bool is_enabled(coreutil_config const& config, std::string_view name) {
	return std::find(config.disabled.begin(), config.disabled.end(), name) == config.disabled.end();
}

// -- shared helpers --------------------------------------------------------

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

fs::path resolve_path(araya::session::session_store& store, std::string const& session, std::string_view raw) {
	fs::path path{std::string(raw)};
	if (path.is_absolute())
		return path;
	fs::path base = fs::current_path();
	if (auto s = store.get(araya::session::session_id{session}); s && s->header().cwd)
		base = *s->header().cwd;
	return base / path;
}

araya::fs::fs_target make_target(std::string const& session, fs::path const& path, std::string const& display) {
	return araya::fs::fs_target{
		.owner = session, .path_key = path.lexically_normal().generic_string(), .display_path = display};
}

// Publish one presence/absence observation on the fs firehose. A no-op when
// no bus is bound (the tool then behaves unconstrained, as if no policy).
void emit_observed(
	std::shared_ptr<araya::event_bus> const& bus,
	araya::fs::fs_target target,
	araya::fs::fs_observation observation) {
	if (!bus)
		return;
	bus->dispatch(araya::fs::observed_key, araya::fs::fs_observed_msg{std::move(target), std::move(observation)});
}

std::vector<std::string> split_lines(std::string const& data) {
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start < data.size()) {
		std::size_t const newline = data.find('\n', start);
		std::size_t const end = newline == std::string::npos ? data.size() : newline;
		std::string line = data.substr(start, end - start);
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		lines.push_back(std::move(line));
		if (newline == std::string::npos)
			break;
		start = newline + 1;
	}
	return lines;
}

bool read_file(std::string const& display, fs::path const& path, std::string& out, std::string& error) {
	std::error_code ec;
	if (!fs::exists(path, ec)) {
		error = "Error: file not found: " + display;
		return false;
	}
	if (fs::is_directory(path, ec)) {
		error = "Error: path is a directory, not a file: " + display;
		return false;
	}
	std::ifstream stream(path, std::ios::binary);
	if (!stream) {
		error = "Error: cannot open file: " + display;
		return false;
	}
	std::ostringstream buffer;
	buffer << stream.rdbuf();
	out = buffer.str();
	return true;
}

bool is_binary(std::string const& data) {
	auto const window = std::min<std::size_t>(data.size(), 8192);
	return data.substr(0, window).find('\0') != std::string::npos;
}

// -- glob ------------------------------------------------------------------

std::string escaped(char c) {
	switch (c) {
	case '.':
	case '+':
	case '(':
	case ')':
	case '|':
	case '^':
	case '$':
	case '\\':
	case ']':
	case '}':
		return std::string("\\") + c;
	default:
		return std::string(1, c);
	}
}

std::string glob_fragment_to_regex(std::string_view glob) {
	std::string re;
	for (std::size_t i = 0; i < glob.size(); ++i) {
		char const c = glob[i];
		if (c == '*') {
			if (i + 1 < glob.size() && glob[i + 1] == '*') {
				++i;
				if (i + 1 < glob.size() && glob[i + 1] == '/') {
					++i;
					re += "(?:.*/)?";
				} else {
					re += ".*";
				}
			} else {
				re += "[^/]*";
			}
		} else if (c == '?') {
			re += "[^/]";
		} else if (c == '[') {
			std::size_t j = i + 1;
			std::string cls = "[";
			if (j < glob.size() && (glob[j] == '!' || glob[j] == '^')) {
				cls += '^';
				++j;
			}
			if (j < glob.size() && glob[j] == ']') {
				cls += "\\]";
				++j;
			}
			while (j < glob.size() && glob[j] != ']') {
				if (glob[j] == '\\')
					cls += "\\\\";
				else
					cls += glob[j];
				++j;
			}
			if (j < glob.size() && glob[j] == ']') {
				cls += ']';
				re += cls;
				i = j;
			} else {
				re += "\\[";
			}
		} else {
			re += escaped(c);
		}
	}
	return re;
}

void expand_braces(std::string const& pattern, std::vector<std::string>& out) {
	auto const open = pattern.find('{');
	if (open == std::string::npos) {
		out.push_back(pattern);
		return;
	}
	std::size_t depth = 1;
	std::size_t close = open + 1;
	for (; close < pattern.size() && depth > 0; ++close) {
		if (pattern[close] == '{')
			++depth;
		else if (pattern[close] == '}')
			--depth;
	}
	if (depth != 0) {
		out.push_back(pattern);
		return;
	}
	--close; // index of the matching '}'
	std::string const prefix = pattern.substr(0, open);
	std::string const suffix = pattern.substr(close + 1);
	std::string inner = pattern.substr(open + 1, close - open - 1);
	std::vector<std::string> options;
	std::string part;
	int nested = 0;
	for (char const c : inner) {
		if (c == ',' && nested == 0) {
			options.push_back(part);
			part.clear();
		} else {
			if (c == '{')
				++nested;
			if (c == '}')
				--nested;
			part += c;
		}
	}
	options.push_back(part);
	for (auto const& option : options)
		expand_braces(prefix + option + suffix, out);
}

bool glob_match(std::string const& pattern, std::string const& target) {
	std::vector<std::string> expanded;
	expand_braces(pattern, expanded);
	for (auto const& one : expanded) {
		try {
			std::regex const re("^" + glob_fragment_to_regex(one) + "$", std::regex::ECMAScript);
			if (std::regex_match(target, re))
				return true;
		} catch (std::regex_error const&) {
			return false;
		}
	}
	return false;
}

bool is_vcs(std::string const& name) { return name == ".git" || name == ".hg" || name == ".svn"; }

// Walk `root` collecting regular files, excluding VCS metadata directories.
std::vector<fs::path> collect_files(fs::path const& root) {
	std::vector<fs::path> files;
	std::error_code ec;
	fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
	fs::recursive_directory_iterator const end;
	for (; it != end; it.increment(ec)) {
		if (ec) {
			ec.clear();
			continue;
		}
		auto const name = it->path().filename().string();
		if (it->is_directory(ec)) {
			if (is_vcs(name))
				it.disable_recursion_pending();
			continue;
		}
		if (it->is_regular_file(ec))
			files.push_back(it->path());
	}
	return files;
}

std::string relative_display(fs::path const& root, fs::path const& file) {
	auto rel = file.lexically_relative(root);
	if (rel.empty())
		return file.string();
	return rel.generic_string();
}

// -- tools -----------------------------------------------------------------

araya::task<tool_result> handle_read(
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::event_bus> bus,
	coreutil_config config,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto file_path = args ? araya::util::json::get_string(*args, "file_path") : std::string{};
	if (file_path.empty())
		co_return error_result("Error: read requires a non-empty 'file_path'");

	auto option = [&](std::string_view key) -> std::optional<std::int64_t> {
		return args ? araya::util::json::opt_int(*args, key) : std::nullopt;
	};
	std::int64_t offset = option("offset").value_or(1);
	std::int64_t limit = option("limit").value_or(static_cast<std::int64_t>(config.read_limit));
	if (offset < 1 || limit < 1)
		co_return error_result("Error: 'offset' and 'limit' must be positive integers");
	if (static_cast<std::size_t>(limit) > config.read_limit)
		co_return error_result("Error: 'limit' must be at most " + std::to_string(config.read_limit));

	auto const path = resolve_path(*store, ctx.session, file_path);
	auto const target = make_target(ctx.session, path, file_path);
	// A miss records confirmed absence before reporting the error; a hit
	// records presence with its version (the policy's freshness basis).
	std::error_code rec;
	if (!fs::exists(path, rec) || rec)
		emit_observed(bus, target, araya::fs::fs_observation{false, {}});

	std::string data;
	std::string error;
	if (!read_file(file_path, path, data, error))
		co_return error_result(std::move(error));
	emit_observed(bus, target, araya::fs::fs_observation{true, araya::fs::read_version(path).value_or("")});

	auto lines = split_lines(data);
	auto const total = lines.size();
	auto const start = static_cast<std::size_t>(offset - 1);
	auto const end = std::min(total, start + static_cast<std::size_t>(limit));

	std::string body;
	if (start < total) {
		for (std::size_t i = start; i < end; ++i) {
			std::string text = lines[i];
			if (text.size() > config.read_max_line)
				text.resize(config.read_max_line);
			if (!body.empty())
				body += '\n';
			body += std::to_string(i + 1) + ": " + text;
		}
	}

	std::string footer;
	if (end < total)
		footer = "(Showing lines " + std::to_string(start + 1) + "-" + std::to_string(end) + " of " +
				 std::to_string(total) + ". Use offset=" + std::to_string(end + 1) + " to continue.)";
	else
		footer = "(End of file - total " + std::to_string(total) + " lines)";

	std::string content;
	content += "<path>" + file_path + "</path>\n<type>file</type>\n<content>\n";
	content += body.empty() ? footer : body + "\n\n" + footer;
	content += "\n</content>";
	co_return text_result(std::move(content));
}

araya::task<tool_result> handle_write(
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::event_bus> bus,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto file_path = args ? araya::util::json::get_string(*args, "file_path") : std::string{};
	auto content = args ? araya::util::json::get_string(*args, "content") : std::string{};
	if (file_path.empty())
		co_return error_result("Error: write requires a non-empty 'file_path'");

	auto const path = resolve_path(*store, ctx.session, file_path);
	auto const target = make_target(ctx.session, path, file_path);
	std::error_code ec;
	bool const existed = fs::is_regular_file(path, ec);

	// The policy's write guard: create-only for an unseen/absent target,
	// version-guarded replacement for an observed one. No listener leaves
	// the intent unconstrained - the bare create-or-overwrite.
	araya::fs::fs_write_intent intent;
	intent.target = target;
	if (bus)
		intent = co_await bus->dispatch(araya::fs::write_intent_key, intent);
	if (intent.decision == araya::fs::fs_write_decision::create_if_absent && existed)
		co_return error_result(
			"cannot modify \"" + file_path + "\": file has not been read — read the file, then retry");
	if (intent.decision == araya::fs::fs_write_decision::replace_if_version) {
		auto const current = araya::fs::read_version(path);
		if (!current || *current != intent.version)
			co_return error_result(
				"cannot write \"" + file_path + "\": the file changed on disk — re-read the file, then retry");
	}

	if (auto const parent = path.parent_path(); !parent.empty()) {
		fs::create_directories(parent, ec);
		if (ec)
			co_return error_result("Error: cannot create directory: " + parent.string());
	}
	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		co_return error_result("Error: cannot write file: " + file_path);
	stream << content;
	stream.close();
	if (!stream)
		co_return error_result("Error: write failed: " + file_path);
	emit_observed(bus, target, araya::fs::fs_observation{true, araya::fs::read_version(path).value_or("")});

	co_return text_result(
		"<path>" + file_path + "</path>\n<type>file</type>\n<content>\n" + (existed ? "Updated file" : "Created file") +
		"\n</content>");
}

araya::task<tool_result> handle_edit(
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::event_bus> bus,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto file_path = args ? araya::util::json::get_string(*args, "file_path") : std::string{};
	auto old_string = args ? araya::util::json::get_string(*args, "old_string") : std::string{};
	auto new_string = args ? araya::util::json::get_string(*args, "new_string") : std::string{};
	bool const replace_all = args && araya::util::json::get_bool(*args, "replace_all");
	if (file_path.empty())
		co_return error_result("Error: edit requires a non-empty 'file_path'");
	if (old_string.empty())
		co_return error_result("Error: 'old_string' must be a non-empty string");
	if (old_string == new_string)
		co_return error_result("Error: 'old_string' and 'new_string' must differ");

	auto const path = resolve_path(*store, ctx.session, file_path);
	auto const target = make_target(ctx.session, path, file_path);

	// The policy's edit guard: an unseen or absent target is rejected; a
	// present one carries the observed version for a guarded replacement.
	araya::fs::fs_edit_intent intent;
	intent.target = target;
	if (bus)
		intent = co_await bus->dispatch(araya::fs::edit_intent_key, intent);
	if (!intent.error.empty())
		co_return error_result(intent.error);
	if (intent.decision == araya::fs::fs_edit_decision::replace_if_version) {
		auto const current = araya::fs::read_version(path);
		if (!current || *current != intent.version)
			co_return error_result(
				"cannot edit \"" + file_path + "\": the file changed on disk — re-read the file, then retry");
	}

	std::string data;
	std::string error;
	if (!read_file(file_path, path, data, error))
		co_return error_result(std::move(error));

	std::size_t count = 0;
	for (std::size_t at = data.find(old_string); at != std::string::npos;
		 at = data.find(old_string, at + old_string.size()))
		++count;
	if (count == 0)
		co_return error_result("Error: 'old_string' was not found in " + file_path);
	if (count > 1 && !replace_all)
		co_return error_result(
			"Error: 'old_string' appears " + std::to_string(count) + " times in " + file_path +
			"; provide a more specific old_string or set replace_all to true");

	std::string updated;
	std::size_t cursor = 0;
	for (std::size_t at = data.find(old_string); at != std::string::npos;
		 at = data.find(old_string, at + old_string.size())) {
		updated.append(data, cursor, at - cursor);
		updated += new_string;
		cursor = at + old_string.size();
		if (!replace_all)
			break;
	}
	updated.append(data, cursor, data.size() - cursor);

	std::ofstream stream(path, std::ios::binary | std::ios::trunc);
	if (!stream)
		co_return error_result("Error: cannot write file: " + file_path);
	stream << updated;
	stream.close();
	if (!stream)
		co_return error_result("Error: edit failed: " + file_path);
	emit_observed(bus, target, araya::fs::fs_observation{true, araya::fs::read_version(path).value_or("")});

	co_return text_result(
		replace_all ? "The file " + file_path + " has been updated. All occurrences were successfully replaced."
					: "The file " + file_path + " has been updated successfully.");
}

// -- ripgrep backend + spill -----------------------------------------------

std::string shell_quote(std::string const& value) {
	std::string out = "'";
	for (char const c : value) {
		if (c == '\'')
			out += "'\\''";
		else
			out.push_back(c);
	}
	out += "'";
	return out;
}

// Whether a `rg` binary is on PATH. Result is cached for the process.
bool rg_available() {
	static bool const available = [] {
		auto const* path = std::getenv("PATH");
		if (!path)
			return false;
		std::string_view view(path);
		std::size_t start = 0;
		while (start <= view.size()) {
			auto const end = view.find(':', start);
			auto const dir = view.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
			std::error_code ec;
			if (!dir.empty() && fs::is_regular_file(fs::path(std::string(dir)) / "rg", ec))
				return true;
			if (end == std::string_view::npos)
				break;
			start = end + 1;
		}
		return false;
	}();
	return available;
}

struct command_result {
	int status = -1; // exit code, or -1 on failure to run
	std::string output;
	bool truncated = false;
};

// Run a shell command synchronously and capture its stdout up to `cap` bytes.
// The command string is built entirely from shell_quote'd arguments.
command_result run_command(std::string const& command, std::size_t cap) {
	command_result result;
	FILE* pipe = ::popen(command.c_str(), "r");
	if (!pipe)
		return result;
	std::array<char, 16384> buffer{};
	for (;;) {
		auto const read = std::fread(buffer.data(), 1, buffer.size(), pipe);
		if (read == 0)
			break;
		if (result.output.size() < cap) {
			auto const keep = std::min<std::size_t>(read, cap - result.output.size());
			result.output.append(buffer.data(), keep);
			if (keep < read)
				result.truncated = true;
		} else {
			result.truncated = true;
		}
	}
	auto const code = ::pclose(pipe);
	if (code != -1 && WIFEXITED(code))
		result.status = WEXITSTATUS(code);
	return result;
}

// Persist complete search output to a file and return its path.
std::optional<fs::path> write_spill(std::string const& text, std::string const& kind) {
	std::error_code ec;
	auto root = fs::temp_directory_path(ec) / "araya-spill";
	if (ec)
		return std::nullopt;
	fs::create_directories(root, ec);
	if (ec)
		return std::nullopt;
	auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
	auto const path = root / (kind + "-" + std::to_string(stamp) + ".txt");
	std::ofstream stream(path, std::ios::binary);
	if (!stream)
		return std::nullopt;
	stream << text;
	return path;
}

araya::task<tool_result>
handle_glob(std::shared_ptr<araya::session::session_store> store, coreutil_config config, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto pattern = args ? araya::util::json::get_string(*args, "pattern") : std::string{};
	if (pattern.empty())
		co_return error_result("Error: glob requires a non-empty 'pattern'");
	auto const raw_path = args ? araya::util::json::get_string(*args, "path") : std::string{};

	fs::path const root =
		raw_path.empty() ? resolve_path(*store, ctx.session, ".") : resolve_path(*store, ctx.session, raw_path);
	std::error_code ec;
	if (!fs::is_directory(root, ec))
		co_return error_result("Error: glob path is not a directory: " + root.string());

	bool const basename_only = pattern.find('/') == std::string::npos;
	std::vector<fs::path> matches;
	for (auto const& file : collect_files(root)) {
		auto const target = basename_only ? file.filename().string() : relative_display(root, file);
		if (glob_match(pattern, target))
			matches.push_back(file);
	}
	std::sort(matches.begin(), matches.end(), [](fs::path const& a, fs::path const& b) {
		std::error_code ea;
		std::error_code eb;
		return fs::last_write_time(a, ea) > fs::last_write_time(b, eb);
	});

	std::string body;
	auto const shown = std::min(matches.size(), config.glob_max);
	for (std::size_t i = 0; i < shown; ++i) {
		if (i > 0)
			body += '\n';
		body += relative_display(root, matches[i]);
	}
	if (matches.empty())
		co_return text_result("No files found");
	if (matches.size() > config.glob_max) {
		std::string full;
		for (auto const& match : matches)
			full += relative_display(root, match) + "\n";
		body += "\n\n(Showing " + std::to_string(shown) + " of " + std::to_string(matches.size()) +
				" paths in modification-time order; narrow the pattern or path to see more.)";
		if (auto path = write_spill(full, "glob"))
			body += "\nComplete result: " + path->string();
	}
	co_return text_result(std::move(body));
}

araya::task<tool_result>
handle_grep(std::shared_ptr<araya::session::session_store> store, coreutil_config config, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto pattern = args ? araya::util::json::get_string(*args, "pattern") : std::string{};
	if (pattern.empty())
		co_return error_result("Error: grep requires a non-empty 'pattern'");
	auto const raw_path = args ? araya::util::json::get_string(*args, "path") : std::string{};
	auto const include = args ? araya::util::json::get_string(*args, "include") : std::string{};

	fs::path const root =
		raw_path.empty() ? resolve_path(*store, ctx.session, ".") : resolve_path(*store, ctx.session, raw_path);
	std::error_code ec;
	if (!fs::exists(root, ec))
		co_return error_result("Error: grep path does not exist: " + root.string());

	// Prefer the real ripgrep backend when available (ripgrep regex syntax,
	// ignore-file aware, fast); fall back to the native ECMAScript matcher.
	if (rg_available()) {
		std::string command = "rg --line-number --no-heading --with-filename --color never";
		command += " -e " + shell_quote(pattern);
		if (!include.empty())
			command += " -g " + shell_quote(include);
		command += " -- " + shell_quote(root.string()) + " 2>/dev/null";
		auto const rg = run_command(command, std::max<std::size_t>(config.spill_bytes, 4 * 1024 * 1024));
		if (rg.status == 0 || rg.status == 1) {
			if (rg.status == 1)
				co_return text_result("No matches found");
			struct rg_match {
				std::string path;
				std::size_t line;
				std::string text;
			};
			std::vector<rg_match> matches;
			std::size_t seen = 0;
			bool capped = rg.truncated;
			std::size_t start = 0;
			while (start <= rg.output.size()) {
				auto const end = rg.output.find('\n', start);
				auto const line = std::string_view(rg.output).substr(
					start, end == std::string::npos ? std::string_view::npos : end - start);
				start = end == std::string::npos ? rg.output.size() + 1 : end + 1;
				if (line.empty())
					continue;
				auto const first = line.find(':');
				if (first == std::string_view::npos)
					continue;
				auto const second = line.find(':', first + 1);
				if (second == std::string_view::npos)
					continue;
				++seen;
				if (matches.size() >= config.grep_max) {
					capped = true;
					continue;
				}
				auto const file_path = std::string(line.substr(0, first));
				auto const line_no =
					std::strtoull(std::string(line.substr(first + 1, second - first - 1)).c_str(), nullptr, 10);
				std::string text(line.substr(second + 1));
				if (text.size() > config.grep_max_line)
					text.resize(config.grep_max_line);
				auto const display = fs::is_regular_file(root, ec) ? (raw_path.empty() ? file_path : raw_path)
																   : relative_display(root, fs::path(file_path));
				matches.push_back(rg_match{display, static_cast<std::size_t>(line_no), std::move(text)});
			}
			if (seen == 0)
				co_return text_result("No matches found");
			std::string body;
			std::string current;
			for (auto const& m : matches) {
				if (m.path != current) {
					if (!body.empty())
						body += "\n\n";
					body += m.path;
					current = m.path;
				}
				body += "\nLine " + std::to_string(m.line) + ": " + m.text;
			}
			std::string const header =
				capped ? "Found " + std::to_string(matches.size()) + " of " + std::to_string(seen) + " matches"
					   : "Found " + std::to_string(seen) + (seen == 1 ? " match" : " matches");
			std::string out = header + "\n\n" + body;
			if (capped) {
				out += "\n\n(Output truncated; narrow the pattern, path, or include to see more.)";
				if (auto path = write_spill(rg.output, "grep"))
					out += "\nComplete result: " + path->string();
			}
			co_return text_result(std::move(out));
		}
	}

	std::regex re;
	try {
		re = std::regex(pattern, std::regex::ECMAScript);
	} catch (std::regex_error const& e) {
		co_return error_result(std::string("Error: invalid regular expression: ") + e.what());
	}

	std::vector<fs::path> files;
	if (fs::is_regular_file(root, ec)) {
		files.push_back(root);
	} else {
		files = collect_files(root);
	}

	struct match {
		std::string path;
		std::size_t line;
		std::string text;
	};
	std::vector<match> all;
	std::size_t seen = 0;
	bool capped = false;
	for (auto const& file : files) {
		if (!include.empty() && !glob_match(include, file.filename().string()))
			continue;
		std::ifstream stream(file, std::ios::binary);
		if (!stream)
			continue;
		std::ostringstream buffer;
		buffer << stream.rdbuf();
		std::string const data = buffer.str();
		if (is_binary(data))
			continue;
		auto const lines = split_lines(data);
		auto const display = fs::is_regular_file(root, ec) ? raw_path : relative_display(root, file);
		for (std::size_t i = 0; i < lines.size(); ++i) {
			if (!std::regex_search(lines[i], re))
				continue;
			++seen;
			if (all.size() >= config.grep_max) {
				capped = true;
				continue;
			}
			std::string text = lines[i];
			if (text.size() > config.grep_max_line)
				text.resize(config.grep_max_line);
			all.push_back(match{display, i + 1, std::move(text)});
		}
	}

	if (seen == 0)
		co_return text_result("No matches found");

	std::string body;
	std::string current;
	for (auto const& m : all) {
		if (m.path != current) {
			if (!body.empty())
				body += "\n\n";
			body += m.path;
			current = m.path;
		}
		body += "\nLine " + std::to_string(m.line) + ": " + m.text;
	}
	std::string const header = capped
								   ? "Found " + std::to_string(all.size()) + " of " + std::to_string(seen) + " matches"
								   : "Found " + std::to_string(seen) + (seen == 1 ? " match" : " matches");
	std::string out = header + "\n\n" + body;
	if (capped) {
		out += "\n\n(Output truncated; narrow the pattern, path, or include to see more.)";
		if (auto path = write_spill(body, "grep"))
			out += "\nComplete result: " + path->string();
	}
	co_return text_result(std::move(out));
}

// -- schemas ---------------------------------------------------------------

boost::json::value schema(boost::json::object properties, std::initializer_list<char const*> required = {}) {
	boost::json::object out{{"type", "object"}, {"properties", std::move(properties)}};
	if (required.size() > 0) {
		boost::json::array req;
		for (auto const* name : required)
			req.emplace_back(name);
		out["required"] = std::move(req);
	}
	return out;
}

boost::json::object str(char const* description) {
	return boost::json::object{{"type", "string"}, {"description", description}};
}

araya::system_prompt::prompt_section guidance(std::string name, int order, std::string text) {
	araya::system_prompt::prompt_section section;
	section.name = std::move(name);
	section.order = order;
	section.text = std::move(text);
	return section;
}

// -- plugin ----------------------------------------------------------------

struct coreutil_plugin : araya::plugin {
	explicit coreutil_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		// The fs event gate the observation policy listens on. Absent a
		// mounted policy the dispatches have no listeners, so every guard
		// stays unconstrained - the bare create-or-overwrite.
		auto bus = ctx.activation_ptr()->bus;
		auto const& config = config_;

		if (is_enabled(config, "read")) {
			prompts->section(
				ctx,
				guidance(
					"tool:read",
					araya::system_prompt::section_order("TOOL_READ"),
					"Use the read tool — not shell commands like cat — to inspect text files. Results include line "
					"numbers. Use offset and limit to continue reading large files."));
			tools->register_tool(
				ctx,
				tool_definition{
					"read",
					"Read a UTF-8 text file and return line-numbered content.",
					schema(
						{{"file_path", str("Path to read.")},
						 {"offset",
						  boost::json::object{
							  {"type", "number"}, {"description", "1-based first line to return. Defaults to 1."}}},
						 {"limit",
						  boost::json::object{
							  {"type", "number"}, {"description", "Maximum number of lines to return."}}}},
						{"file_path"})},
				[store, bus, config](tool_context const& call) { return handle_read(store, bus, config, call); });
		}

		if (is_enabled(config, "write")) {
			std::string write_text =
				"Use the write tool to create files or completely replace file contents. Existing files are "
				"overwritten, so read an existing file first (the default fs-observation-policy requires it)";
			if (is_enabled(config, "edit"))
				write_text += " and prefer edit for targeted changes";
			write_text += ".";
			prompts->section(
				ctx, guidance("tool:write", araya::system_prompt::section_order("TOOL_WRITE"), std::move(write_text)));
			tools->register_tool(
				ctx,
				tool_definition{
					"write",
					"Create or fully replace a UTF-8 text file.",
					schema(
						{{"file_path", str("Path to write.")}, {"content", str("Full UTF-8 text content to write.")}},
						{"file_path", "content"})},
				[store, bus](tool_context const& call) { return handle_write(store, bus, call); });
		}

		if (is_enabled(config, "edit")) {
			prompts->section(
				ctx,
				guidance(
					"tool:edit",
					araya::system_prompt::section_order("TOOL_EDIT"),
					"Use the edit tool for targeted changes to existing UTF-8 text files. It replaces literal "
					"old_string with new_string; by default old_string must appear exactly once. If old_string "
					"appears multiple times, provide a more specific old_string or set replace_all to true. Read the "
					"file first (the default fs-observation-policy requires it), unless you just created or edited it "
					"in this session."));
			tools->register_tool(
				ctx,
				tool_definition{
					"edit",
					"Edit an existing UTF-8 text file by replacing literal text.",
					schema(
						{{"file_path", str("Path to edit.")},
						 {"old_string", str("Literal text to replace. Must match exactly.")},
						 {"new_string", str("Literal replacement text. Use an empty string to delete the match.")},
						 {"replace_all",
						  boost::json::object{
							  {"type", "boolean"}, {"description", "Replace all matches. Defaults to false."}}}},
						{"file_path", "old_string", "new_string"})},
				[store, bus](tool_context const& call) { return handle_edit(store, bus, call); });
		}

		if (is_enabled(config, "glob")) {
			prompts->section(
				ctx,
				guidance(
					"tool:glob",
					araya::system_prompt::section_order("TOOL_GLOB"),
					"Use the glob tool — not shell find — to discover files by path pattern. A pattern with no \"/\" "
					"matches basenames at any depth, so \"*\" matches every file in the tree rather than its top "
					"level. "
					"Results are files only, never directories, and include hidden and ignored files: a result that "
					"fits comes back in modification-time order, while a larger one keeps the "
					"modification-time-ordered "
					"head."));
			tools->register_tool(
				ctx,
				tool_definition{
					"glob",
					"Find files whose paths match a glob pattern. Returns matching file paths, ordered by modification "
					"time.",
					schema(
						{{"pattern", str("Glob pattern to match file paths against (e.g. \"**/*.ts\").")},
						 {"path", str("Directory to search in. Defaults to the session workspace.")}},
						{"pattern"})},
				[store, config](tool_context const& call) { return handle_glob(store, config, call); });
		}

		if (is_enabled(config, "grep")) {
			std::string grep_text = "Use the grep tool — not shell grep or rg — to search file contents.";
			if (is_enabled(config, "read"))
				grep_text += " Use read on a matched file when you need surrounding context.";
			prompts->section(
				ctx, guidance("tool:grep", araya::system_prompt::section_order("TOOL_GREP"), std::move(grep_text)));
			tools->register_tool(
				ctx,
				tool_definition{
					"grep",
					"Search file contents with a regular expression. Returns matching lines with line numbers, grouped "
					"by file.",
					schema(
						{{"pattern", str("Regular expression to search for.")},
						 {"path", str("File or directory to search. Defaults to the session workspace.")},
						 {"include", str("One glob filter for which files to search (e.g. \"*.ts\").")}},
						{"pattern"})},
				[store, config](tool_context const& call) { return handle_grep(store, config, call); });
		}
		co_return;
	}

private:
	coreutil_config config_;
};

std::unique_ptr<araya::plugin> make_coreutil(araya::plugin_config const& config) {
	return std::make_unique<coreutil_plugin>(config);
}

static const araya::dependency_spec g_coreutil_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_coreutil_provs{};
static const araya::plugin_descriptor g_descriptor{"coreutil", g_coreutil_deps, g_coreutil_provs, &make_coreutil};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::coreutil
