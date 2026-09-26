#include "araya/tool-jobs/tool_jobs.hpp"

#include "araya/config.hpp"
#include "araya/jobs/jobs.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"
#include "araya/util/json.hpp"

#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <boost/json/value.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace araya::tool_jobs {
namespace {

using araya::jobs::job_snapshot;
using araya::jobs::job_status;
using araya::jobs::jobs_key;
using araya::jobs::jobs_service;
using araya::session::session_id;
using araya::session::session_store;
using araya::session::sessions_key;
using araya::tools::tool_context;
using araya::tools::tool_definition;
using araya::tools::tool_result;
using araya::tools::tools_key;
using araya::tools::tools_service;

constexpr araya::config_key<std::uint64_t> wait_timeout_key{"waitTimeoutMs"};
constexpr araya::config_key<std::uint64_t> max_wait_timeout_key{"maxWaitTimeoutMs"};

struct tool_jobs_config {
	std::uint64_t wait_timeout_ms = 30'000;
	std::uint64_t max_wait_timeout_ms = 600'000;
};

tool_jobs_config parse_config(araya::plugin_config const& config) {
	araya::plugin_config_view const view(config);
	tool_jobs_config out;
	if (auto value = view.try_get(wait_timeout_key))
		out.wait_timeout_ms = *value;
	if (auto value = view.try_get(max_wait_timeout_key))
		out.max_wait_timeout_ms = *value;
	return out;
}

std::optional<std::string> owner_of(tool_context const& ctx) {
	if (ctx.session.empty())
		return std::nullopt;
	return ctx.session;
}

tool_result text_result(std::string text, bool is_error = false) {
	return tool_result{boost::json::array{{{"type", "text"}, {"text", std::move(text)}}}, is_error};
}

tool_result error_result(std::string text) { return text_result(std::move(text), true); }

std::string status_line(job_snapshot const& snapshot) {
	std::string text = "[status: ";
	text += araya::jobs::job_status_name(snapshot.status);
	if (snapshot.detail && !snapshot.detail->empty()) {
		text += ", ";
		text += *snapshot.detail;
	}
	text += "]";
	return text;
}

// The completion notice appended to the owner's session, bounded by the job's
// output cap: the full form, then a compact form, then a hard truncation.
std::string notice_text(job_snapshot const& snapshot) {
	std::string full = "background job " + snapshot.id + " (" + snapshot.kind + ": " + snapshot.label + ") finished " +
					   status_line(snapshot) + ". Read its output with job_output.";
	if (!snapshot.output_limit_bytes || full.size() <= *snapshot.output_limit_bytes)
		return full;
	std::string compact =
		"background job " + snapshot.id + " finished " + status_line(snapshot) + ". Read its output with job_output.";
	if (compact.size() <= *snapshot.output_limit_bytes)
		return compact;
	return compact.substr(0, *snapshot.output_limit_bytes);
}

araya::task<tool_result>
handle_job_output(std::shared_ptr<jobs_service> jobs, tool_jobs_config config, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto job_id = args ? araya::util::json::get_string(*args, "job_id") : std::string{};
	if (job_id.empty())
		co_return error_result("Error: job_output requires a non-empty 'job_id'");
	auto owner = owner_of(ctx);
	try {
		bool const wait = args ? araya::util::json::opt_bool(*args, "wait").value_or(false) : false;
		if (wait) {
			std::uint64_t timeout = config.wait_timeout_ms;
			if (auto requested = args ? araya::util::json::opt_int(*args, "timeout_ms") : std::nullopt) {
				if (*requested <= 0)
					co_return error_result("Error: 'timeout_ms' must be positive");
				timeout = static_cast<std::uint64_t>(*requested);
			}
			timeout = std::min(timeout, config.max_wait_timeout_ms);
			co_await jobs->wait(job_id, timeout, owner, ctx.stop);
		}
		auto read = jobs->read(job_id, owner);
		std::string body = read.text.empty() ? "(no new output)" : read.text;
		if (!body.empty() && body.back() != '\n')
			body += '\n';
		body += status_line(read.snapshot);
		co_return text_result(std::move(body));
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}
}

araya::task<tool_result> handle_job_list(std::shared_ptr<jobs_service> jobs, tool_context const& ctx) {
	try {
		auto list = jobs->list(owner_of(ctx));
		if (list.empty())
			co_return text_result("(no background jobs)");
		std::string body;
		for (auto const& job : list) {
			body +=
				job.id + " [" + job.kind + "] " + araya::jobs::job_status_name(job.status) + " — " + job.label + "\n";
		}
		if (!body.empty() && body.back() == '\n')
			body.pop_back();
		co_return text_result(std::move(body));
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}
}

araya::task<tool_result> handle_job_kill(std::shared_ptr<jobs_service> jobs, tool_context const& ctx) {
	auto const* args = ctx.arguments.if_object();
	auto job_id = args ? araya::util::json::get_string(*args, "job_id") : std::string{};
	if (job_id.empty())
		co_return error_result("Error: job_kill requires a non-empty 'job_id'");
	std::string reason = args ? araya::util::json::opt_string(*args, "reason").value_or("") : std::string{};
	auto owner = owner_of(ctx);
	try {
		auto result = jobs->kill(job_id, owner, reason);
		auto snapshot = jobs->get(job_id, owner);
		if (result == jobs_service::kill_result::already_finished)
			co_return text_result("job " + job_id + " had already finished " + status_line(snapshot));
		co_return text_result("requested cancellation of job " + job_id);
	} catch (std::exception const& e) {
		co_return error_result(std::string("Error: ") + e.what());
	}
}

boost::json::value job_output_schema() {
	return boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"job_id",
			  boost::json::object{
				  {"type", "string"},
				  {"description", "Job id returned by the tool that started the background work."}}},
			 {"wait",
			  boost::json::object{
				  {"type", "boolean"},
				  {"description",
				   "Block until the job reaches a terminal status or the timeout expires. A timed-out wait returns "
				   "[status: running] and leaves the job alive."}}},
			 {"timeout_ms",
			  boost::json::object{
				  {"type", "number"},
				  {"description",
				   "Max wait in milliseconds (only meaningful with wait: true). Defaults to the configured wait "
				   "timeout; capped by the configured maximum."}}}}},
		{"required", boost::json::array{"job_id"}},
	};
}

boost::json::value job_kill_schema() {
	return boost::json::value{
		{"type", "object"},
		{"properties",
		 boost::json::object{
			 {"job_id",
			  boost::json::object{
				  {"type", "string"},
				  {"description", "Job id returned by the tool that started the background work."}}},
			 {"reason",
			  boost::json::object{
				  {"type", "string"},
				  {"description", "Optional short reason, recorded in the log and forwarded to the job."}}}}},
		{"required", boost::json::array{"job_id"}},
	};
}

struct tool_jobs_plugin : araya::plugin {
	explicit tool_jobs_plugin(araya::plugin_config config)
		: config_(parse_config(config)) {
		if (config_.wait_timeout_ms > config_.max_wait_timeout_ms)
			throw araya::config_error(
				"waitTimeoutMs", std::to_string(config_.wait_timeout_ms), "must not exceed maxWaitTimeoutMs");
	}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto jobs = ctx.require<jobs_service>(jobs_key).shared();
		auto store = ctx.require<session_store>(sessions_key).shared();
		auto prompts =
			ctx.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
		auto tools = ctx.require<tools_service>(tools_key).shared();

		// Producers may start work only while a controller is attached.
		jobs->attach_controller(ctx, "tool-jobs");

		{
			araya::system_prompt::prompt_section section;
			section.name = "tool:jobs";
			section.order = araya::system_prompt::section_order("TOOL_JOBS");
			section.text =
				"Track every background job id you start. You are notified in-session when a job finishes — do not "
				"busy-poll or sleep on one; keep working on independent steps and do not duplicate a running job's "
				"work. Before giving a final answer, collect every still-relevant job with job_output (set wait: "
				"true only when you are genuinely blocked on it), and job_kill jobs that stopped mattering.";
			prompts->section(ctx, std::move(section));
		}

		// A settled, unreported job appends a durable notice to its owner's
		// session (the harness's in-session completion notice).
		jobs->on_job_done(ctx, [store](job_snapshot const& snapshot, std::optional<std::string> const& owner) {
			if (snapshot.reported || !owner)
				return;
			auto session = store->get(session_id{*owner});
			if (!session)
				return;
			session->append(
				"user/message",
				araya::llm_bridge::user_message_data(
					"c" + std::to_string(session->log().size()), notice_text(snapshot)));
		});

		auto config = config_;
		tools->register_tool(
			ctx,
			tool_definition{
				"job_output",
				"Read a background job. Stream jobs return only output since the previous read; final-output jobs "
				"return their result after settlement. Every response ends with `[status: ...]`. Reads are "
				"non-blocking unless `wait: true`, which waits up to the configured cap.",
				job_output_schema()},
			[jobs, config](tool_context const& call) { return handle_job_output(jobs, config, call); });

		tools->register_tool(
			ctx,
			tool_definition{
				"job_list",
				"List your background jobs (running and finished) with their ids, kinds, and statuses.",
				boost::json::value{{"type", "object"}, {"properties", boost::json::object{}}}},
			[jobs](tool_context const& call) { return handle_job_list(jobs, call); });

		tools->register_tool(
			ctx,
			tool_definition{
				"job_kill",
				"Request cancellation of a running background job by job id. Returns immediately; the job settles "
				"as killed once its work actually stops.",
				job_kill_schema()},
			[jobs](tool_context const& call) { return handle_job_kill(jobs, call); });
		co_return;
	}

private:
	tool_jobs_config config_;
};

std::unique_ptr<araya::plugin> make_tool_jobs(araya::plugin_config const& config) {
	return std::make_unique<tool_jobs_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"jobs", 1}, true, {}},
	{araya::service_id{"sessions", 1}, true, {}},
	{araya::service_id{"system-prompt", 1}, true, {}},
	{araya::service_id{"tools", 1}, true, {}},
};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"tool-jobs", g_deps, g_provs, &make_tool_jobs};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::tool_jobs
