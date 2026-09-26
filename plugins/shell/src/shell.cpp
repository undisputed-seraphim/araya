#include "araya/shell/shell.hpp"

#include "araya/config.hpp"
#include "araya/effects.hpp"
#include "araya/jobs/jobs.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <sys/wait.h>

namespace araya::shell {
namespace {

namespace bp = boost::process::v2;
namespace fs = std::filesystem;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using boost::asio::awaitable;
using boost::asio::readable_pipe;
using boost::asio::steady_timer;

constexpr araya::config_key<std::uint64_t> timeout_key{"timeout_ms"};
constexpr araya::config_key<std::uint64_t> max_output_key{"max_output_bytes"};
constexpr araya::config_key<std::string> shell_key{"shell"};
constexpr araya::config_key<bool> background_key{"enable_run_in_background"};
constexpr araya::config_key<std::uint64_t> spill_max_key{"spill_max_bytes"};

struct shell_config {
	std::uint64_t timeout_ms = 30000;
	std::size_t max_output = 100'000;
	std::string shell = "/bin/bash";
	bool enable_run_in_background = true;
	// Per-stream cap on the full-output spill file.
	std::size_t spill_max = 64 * 1024 * 1024;
};

shell_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view view(config);
	shell_config out;
	if (auto value = view.try_get(timeout_key))
		out.timeout_ms = *value;
	if (auto value = view.try_get(max_output_key))
		out.max_output = static_cast<std::size_t>(*value);
	if (auto value = view.try_get(shell_key))
		out.shell = *value;
	if (auto value = view.try_get(background_key))
		out.enable_run_in_background = *value;
	if (auto value = view.try_get(spill_max_key))
		out.spill_max = static_cast<std::size_t>(*value);
	return out;
}

struct shell_result {
	bool launched = false;
	bool timed_out = false;
	bool aborted = false;
	std::string stdout_text;
	std::string stderr_text;
	std::size_t stdout_dropped = 0;
	std::size_t stderr_dropped = 0;
	// Paths to the full-output spill files when a stream overflowed.
	std::string stdout_spill;
	std::string stderr_spill;
	int exit_code = 0;
	int signal = 0;
	std::string error;
};

// Shared kill state: the timeout watcher and the caller's stop callback
// both live outside the runner coroutine, so the pid and liveness flag are
// atomics the runner clears before it returns.
struct watch_state {
	std::atomic<bool> alive{true};
	std::atomic<long> pid{0};
	std::atomic<bool> timed_out{false};
};

void kill_process(watch_state& state) {
	if (!state.alive.load())
		return;
	auto const pid = static_cast<pid_t>(state.pid.load());
	if (pid > 0)
		::kill(pid, SIGKILL);
}

// Retains a stream's overflow in a temp file so truncation never loses the
// full output; the file is opened lazily on the first dropped byte and capped
// at `limit`.
struct spill_writer {
	std::string kind;
	std::size_t limit = 0;
	fs::path path;
	std::ofstream stream;
	std::size_t written = 0;
	bool prefix_written = false;

	// Record the already-buffered prefix once, so the spill file is the
	// complete stream (in-memory head plus overflow).
	void write_prefix(std::string const& prefix) {
		if (prefix_written)
			return;
		prefix_written = true;
		write(prefix.data(), prefix.size());
	}

	void write(char const* data, std::size_t size) {
		if (written >= limit || size == 0)
			return;
		if (!stream.is_open()) {
			std::error_code ec;
			auto root = fs::temp_directory_path(ec) / "araya-spill";
			if (ec)
				return;
			fs::create_directories(root, ec);
			if (ec)
				return;
			auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
			path = root / (kind + "-" + std::to_string(stamp) + ".txt");
			stream.open(path, std::ios::binary);
			if (!stream) {
				path.clear();
				return;
			}
		}
		auto const keep = std::min(size, limit - written);
		stream.write(data, static_cast<std::streamsize>(keep));
		written += keep;
	}
};

awaitable<void>
drain(readable_pipe& pipe, std::string& out, std::size_t cap, std::size_t& dropped, spill_writer* spill = nullptr) {
	std::array<char, 16384> buffer{};
	for (;;) {
		boost::system::error_code ec;
		auto const read = co_await pipe.async_read_some(
			boost::asio::buffer(buffer), boost::asio::redirect_error(boost::asio::use_awaitable, ec));
		if (ec)
			break;
		if (out.size() < cap) {
			auto const keep = std::min<std::size_t>(read, cap - out.size());
			out.append(buffer.data(), keep);
			if (keep < read) {
				dropped += read - keep;
				if (spill) {
					spill->write_prefix(out);
					spill->write(buffer.data() + keep, read - keep);
				}
			}
		} else {
			dropped += read;
			if (spill) {
				spill->write_prefix(out);
				spill->write(buffer.data(), read);
			}
		}
	}
	co_return;
}

// A detached background process: its pipes are drained by a coroutine that
// outlives the tool call, and read_output() hands the model the delta since the
// previous read (stderr under a marked section). The producer owns the buffers;
// the jobs registry owns identity and lifecycle.
struct bg_state {
	std::string stdout_text;
	std::string stderr_text;
	std::size_t stdout_cursor = 0;
	std::size_t stderr_cursor = 0;
	std::size_t stdout_dropped = 0;
	std::size_t stderr_dropped = 0;
	std::size_t stdout_dropped_reported = 0;
	std::size_t stderr_dropped_reported = 0;
	std::atomic<bool> alive{true};
	std::atomic<long> pid{0};
	std::atomic<bool> requested_cancel{false};
	std::optional<bp::process> process;

	std::string read_output() {
		std::string text;
		if (stdout_cursor < stdout_text.size()) {
			text.append(stdout_text, stdout_cursor, std::string::npos);
			stdout_cursor = stdout_text.size();
		}
		if (stderr_cursor < stderr_text.size()) {
			if (!text.empty() && text.back() != '\n')
				text += '\n';
			text += "[stderr]\n";
			text.append(stderr_text, stderr_cursor, std::string::npos);
			stderr_cursor = stderr_text.size();
		}
		auto const dropped = (stdout_dropped - stdout_dropped_reported) + (stderr_dropped - stderr_dropped_reported);
		if (dropped > 0) {
			if (!text.empty() && text.back() != '\n')
				text += '\n';
			text += "[output truncated; " + std::to_string(dropped) + " bytes dropped, full output not retained]";
			stdout_dropped_reported = stdout_dropped;
			stderr_dropped_reported = stderr_dropped;
		}
		return text;
	}
};

void kill_background(bg_state& state) {
	if (!state.alive.load())
		return;
	auto const pid = static_cast<pid_t>(state.pid.load());
	if (pid > 0)
		::kill(pid, SIGKILL);
}

araya::task<void> run_background(
	boost::asio::any_io_executor executor,
	std::string shell,
	std::size_t max_output,
	std::string command,
	std::string workdir,
	std::shared_ptr<bg_state> state,
	std::function<void(araya::jobs::job_outcome)> settle) {
	readable_pipe out(executor);
	readable_pipe err(executor);
	try {
		state->process.emplace(
			executor,
			std::move(shell),
			std::initializer_list<std::string>{"-c", std::move(command)},
			bp::process_stdio{.in = nullptr, .out = out, .err = err},
			bp::process_start_dir(bp::filesystem::path(std::move(workdir))));
	} catch (std::exception const& e) {
		state->alive = false;
		settle(araya::jobs::job_outcome{
			araya::jobs::job_status::failed, std::string("failed to launch command: ") + e.what(), {}});
		co_return;
	}
	state->pid = static_cast<long>(state->process->id());

	using namespace boost::asio::experimental::awaitable_operators;
	co_await (
		drain(out, state->stdout_text, max_output, state->stdout_dropped) &&
		drain(err, state->stderr_text, max_output, state->stderr_dropped));

	boost::system::error_code wait_ec;
	co_await state->process->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_ec));
	state->alive = false;
	if (wait_ec) {
		state->process.reset();
		settle(araya::jobs::job_outcome{araya::jobs::job_status::failed, wait_ec.message(), {}});
		co_return;
	}
	auto const raw = state->process->native_exit_code();
	state->process.reset();
	if (state->requested_cancel.load()) {
		std::string detail = WIFSIGNALED(raw) ? "signal: " + std::to_string(WTERMSIG(raw)) : "killed before exit";
		settle(araya::jobs::job_outcome{araya::jobs::job_status::killed, std::move(detail), {}});
	} else if (WIFEXITED(raw)) {
		settle(araya::jobs::job_outcome{
			araya::jobs::job_status::completed, "exit code: " + std::to_string(WEXITSTATUS(raw)), {}});
	} else if (WIFSIGNALED(raw)) {
		settle(araya::jobs::job_outcome{
			araya::jobs::job_status::completed, "signal: " + std::to_string(WTERMSIG(raw)), {}});
	} else {
		settle(araya::jobs::job_outcome{araya::jobs::job_status::completed, {}, {}});
	}
}

awaitable<shell_result> run_shell(
	boost::asio::any_io_executor executor,
	shell_config config,
	std::string command,
	std::string workdir,
	std::stop_token stop) {
	shell_result result;
	readable_pipe out(executor);
	readable_pipe err(executor);

	std::optional<bp::process> process;
	try {
		process.emplace(
			executor,
			config.shell,
			std::initializer_list<std::string>{"-c", std::move(command)},
			bp::process_stdio{.in = nullptr, .out = out, .err = err},
			bp::process_start_dir(bp::filesystem::path(workdir)));
	} catch (std::exception const& e) {
		result.error = e.what();
		co_return result;
	}
	result.launched = true;

	auto state = std::make_shared<watch_state>();
	state->pid = static_cast<long>(process->id());

	// Timeout watcher: killed from a sibling coroutine on the same strand.
	auto timer = std::make_shared<steady_timer>(executor);
	timer->expires_after(std::chrono::milliseconds(config.timeout_ms));
	boost::asio::co_spawn(
		executor,
		[timer, state]() -> awaitable<void> {
			boost::system::error_code ec;
			co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			if (!ec && state->alive.load()) {
				state->timed_out = true;
				kill_process(*state);
			}
		},
		boost::asio::detached);

	// Caller cancellation: kill the child and let the pipes close.
	std::stop_callback cancel(stop, [state] { kill_process(*state); });

	spill_writer stdout_spill;
	stdout_spill.kind = "stdout";
	stdout_spill.limit = config.spill_max;
	spill_writer stderr_spill;
	stderr_spill.kind = "stderr";
	stderr_spill.limit = config.spill_max;
	using namespace boost::asio::experimental::awaitable_operators;
	co_await (
		drain(out, result.stdout_text, config.max_output, result.stdout_dropped, &stdout_spill) &&
		drain(err, result.stderr_text, config.max_output, result.stderr_dropped, &stderr_spill));
	result.stdout_spill = stdout_spill.path.string();
	result.stderr_spill = stderr_spill.path.string();

	boost::system::error_code wait_ec;
	co_await process->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, wait_ec));
	state->alive = false;
	timer->cancel();
	result.timed_out = state->timed_out.load();
	result.aborted = stop.stop_requested();
	if (wait_ec) {
		result.error = wait_ec.message();
		co_return result;
	}
	// async_wait delivers the EVALUATED code, which cannot distinguish a
	// normal exit from a signal; the handle keeps the raw wait status.
	auto const raw = process->native_exit_code();
	if (WIFEXITED(raw))
		result.exit_code = WEXITSTATUS(raw);
	else if (WIFSIGNALED(raw))
		result.signal = WTERMSIG(raw);
	co_return result;
}

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

std::string truncation_notice(std::size_t dropped) {
	return "[output truncated; " + std::to_string(dropped) + " bytes dropped, full output not retained]";
}

araya::task<tool_result> handle_bash(
	boost::asio::any_io_executor executor,
	std::shared_ptr<araya::session::session_store> store,
	std::shared_ptr<araya::jobs::jobs_service> jobs,
	std::shared_ptr<std::vector<std::weak_ptr<bg_state>>> live,
	shell_config config,
	tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto command = args ? araya::util::json::get_string(*args, "command") : std::string{};
	if (command.empty())
		co_return text_result("Error: bash requires a non-empty 'command'", true);

	std::string workdir;
	if (auto requested = args ? araya::util::json::opt_string(*args, "workdir") : std::nullopt) {
		std::filesystem::path path{*requested};
		if (path.is_relative()) {
			std::filesystem::path base = std::filesystem::current_path();
			if (auto s = store->get(araya::session::session_id{ctx.session}); s && s->header().cwd)
				base = *s->header().cwd;
			path = base / path;
		}
		workdir = path.string();
	} else if (auto s = store->get(araya::session::session_id{ctx.session}); s && s->header().cwd) {
		workdir = *s->header().cwd;
	} else {
		workdir = std::filesystem::current_path().string();
	}
	std::error_code ec;
	if (!std::filesystem::is_directory(workdir, ec))
		co_return text_result("Error: workdir is not a directory: " + workdir, true);

	if (auto timeout = args ? araya::util::json::opt_int(*args, "timeoutMs") : std::nullopt) {
		if (*timeout <= 0)
			co_return text_result("Error: 'timeoutMs' must be positive", true);
		config.timeout_ms = static_cast<std::uint64_t>(*timeout);
	}

	if (args && araya::util::json::opt_bool(*args, "run_in_background").value_or(false)) {
		if (!config.enable_run_in_background)
			co_return text_result("Error: run_in_background is disabled for this deployment", true);
		if (!jobs)
			co_return text_result("Error: background jobs unavailable: load the jobs plugin", true);
		if (ctx.stop.stop_requested())
			co_return text_result("Error: tool call aborted before dispatch", true);

		// The job outlives this tool call: the detached runner drains the pipes
		// and settles through the registry, and cancellation is the job's, not
		// the turn's stop token.
		auto state = std::make_shared<bg_state>();
		live->push_back(state);
		auto label = command;
		auto shell = config.shell;
		auto max_output = config.max_output;
		std::string job_id;
		try {
			job_id = jobs->start(araya::jobs::job_start{
				.kind = "bash",
				.label = std::move(label),
				.owner_session = ctx.session.empty() ? std::nullopt : std::optional<std::string>{ctx.session},
				.output_limit_bytes = std::optional<std::size_t>{max_output},
				.run =
					[executor, shell, max_output, command, workdir, state](
						std::function<void(araya::jobs::job_outcome)> settle) {
						araya::jobs::job_handle handle{
							.cancel =
								[state](std::string const&) {
									state->requested_cancel = true;
									kill_background(*state);
								},
							.read_output = [state] { return state->read_output(); },
						};
						boost::asio::co_spawn(
							executor,
							run_background(executor, shell, max_output, command, workdir, state, std::move(settle)),
							boost::asio::detached);
						return handle;
					},
			});
		} catch (std::exception const& e) {
			co_return text_result(std::string("Error: ") + e.what(), true);
		}
		co_return text_result("started background job " + job_id);
	}

	auto result = co_await run_shell(executor, config, std::move(command), std::move(workdir), ctx.stop);
	if (!result.launched)
		co_return text_result("Error: failed to launch command: " + result.error, true);
	if (result.aborted)
		co_return text_result("Error: command aborted", true);
	if (!result.error.empty())
		co_return text_result("Error: " + result.error, true);

	std::string body = result.stdout_text;
	if (!result.stderr_text.empty()) {
		if (!body.empty() && body.back() != '\n')
			body += '\n';
		body += "[stderr]\n" + result.stderr_text;
	}
	if (body.empty())
		body = "(no output)";

	std::vector<std::string> markers;
	if (result.stdout_dropped > 0 || result.stderr_dropped > 0)
		markers.push_back(truncation_notice(result.stdout_dropped + result.stderr_dropped));
	if (!result.stdout_spill.empty())
		markers.push_back("[full stdout: " + result.stdout_spill + "]");
	if (!result.stderr_spill.empty())
		markers.push_back("[full stderr: " + result.stderr_spill + "]");
	if (result.timed_out)
		markers.push_back("[timed out after " + std::to_string(config.timeout_ms) + "ms]");
	if (result.signal != 0)
		markers.push_back("[killed by signal: " + std::to_string(result.signal) + "]");
	else if (result.exit_code != 0)
		markers.push_back("[exit code: " + std::to_string(result.exit_code) + "]");

	for (auto const& marker : markers) {
		if (!body.empty() && body.back() != '\n')
			body += '\n';
		body += marker;
		if (body.back() != '\n')
			continue;
	}
	co_return text_result(std::move(body));
}

struct shell_plugin : araya::plugin {
	explicit shell_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tools = ctx.require<araya::tools::tools_service>(araya::tools::tools_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto store = ctx.require<araya::session::session_store>(araya::session::sessions_key).shared();
		auto jobs = ctx.find<araya::jobs::jobs_service>(araya::jobs::jobs_key)
						.transform([](auto lease) { return lease.shared(); })
						.value_or(nullptr);
		auto executor = ctx.executor();
		auto config = config_;

		// Background processes outlive a plugin reload only if something kills
		// them; teardown cancels every live one (the jobs registry also cancels
		// the jobs it owns, but shell may unload first).
		auto live = std::make_shared<std::vector<std::weak_ptr<bg_state>>>();
		ctx.effect([live]() -> araya::cleanup_action {
			return [live] {
				for (auto const& weak : *live) {
					if (auto state = weak.lock()) {
						state->requested_cancel = true;
						kill_background(*state);
					}
				}
			};
		});

		{
			araya::system_prompt::prompt_section section;
			section.name = "tool:bash";
			section.order = araya::system_prompt::section_order("TOOL_BASH");
			section.text =
				"Check the [exit code: N] marker on every bash result; investigate failures before moving on.";
			prompts->section(ctx, std::move(section));
		}

		boost::json::object properties{
			{"command", boost::json::object{{"type", "string"}, {"description", "The shell command to execute."}}},
			{"description",
			 boost::json::object{
				 {"type", "string"},
				 {"description",
				  "Clear, concise description of what this command does in active voice (shown in the UI)."}}},
			{"timeoutMs",
			 boost::json::object{
				 {"type", "number"}, {"description", "Timeout in milliseconds; the command is killed on expiry."}}},
			{"workdir",
			 boost::json::object{
				 {"type", "string"}, {"description", "Working directory. Defaults to the session workspace."}}},
		};

		bool const background_enabled = config.enable_run_in_background && jobs != nullptr;
		if (background_enabled) {
			properties["run_in_background"] = boost::json::object{
				{"type", "boolean"},
				{"description",
				 "Run in the background and return a job id immediately (collect with job_output, stop with "
				 "job_kill). No timeout applies."}};
		}
		std::string description =
			"Execute a bash command and return its combined output with [exit code: N], signal, or timeout "
			"markers. "
			"Each call uses a fresh shell, so state does not persist across calls.";
		if (background_enabled)
			description +=
				" Set `run_in_background: true` for long-running commands: the call returns a job id immediately; "
				"read its output with `job_output` and stop it with `job_kill`.";

		tools->register_tool(
			ctx,
			tool_definition{
				"bash",
				std::move(description),
				boost::json::value{
					{"type", "object"},
					{"properties", std::move(properties)},
					{"required", boost::json::array{"command"}}}},
			[executor, store, jobs, live, config](tool_context const& call) {
				return handle_bash(executor, store, jobs, live, config, call);
			});
		co_return;
	}

private:
	shell_config config_;
};

std::unique_ptr<araya::plugin> make_shell(araya::plugin_config const& config) {
	return std::make_unique<shell_plugin>(config);
}

static const araya::dependency_spec g_shell_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
	// Optional: background `run_in_background` needs the jobs registry, but
	// foreground bash works without it.
	{araya::service_id{"jobs", 1}, false, {}},
};
static constexpr std::span<araya::provision_spec const> g_shell_provs{};
static const araya::plugin_descriptor g_descriptor{"shell", g_shell_deps, g_shell_provs, &make_shell};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::shell
