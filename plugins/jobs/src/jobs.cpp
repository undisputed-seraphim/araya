#include "araya/jobs/jobs.hpp"

#include "araya/config.hpp"
#include "araya/session/events.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace araya::jobs {
namespace {

constexpr araya::config_key<std::uint64_t> max_concurrent_key{"max_concurrent_jobs_per_owner"};

std::int64_t now_ms() {
	using namespace std::chrono;
	return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// One live wait: settlement (or teardown) cancels the timer, which resumes the
// waiter coroutine with operation_aborted; the timer expiring naturally is the
// timeout.
struct wait_token {
	std::shared_ptr<boost::asio::steady_timer> timer;
	bool active = true;
};

// The registry's mutable per-job record (never handed out; snapshots are
// fresh projections).
struct tracked_job {
	job_id id;
	std::string kind;
	std::string label;
	std::optional<std::size_t> output_limit_bytes;
	std::optional<std::string> owner_session;
	job_status status = job_status::running;
	std::optional<std::string> detail;
	std::string output;
	std::int64_t started_at_ms = 0;
	std::optional<std::int64_t> finished_at_ms;
	bool reported = false;
	// False once owner disposal or teardown has removed the record; the
	// producer may still settle it, but nothing observable happens.
	bool in_store = false;
	std::function<void(std::string const&)> cancel;
	std::function<std::string()> read_output;
	std::vector<std::shared_ptr<wait_token>> waiters;
};

class job_registry : public jobs_service, public std::enable_shared_from_this<job_registry> {
public:
	job_registry(boost::asio::any_io_executor executor, std::size_t max_concurrent)
		: executor_(std::move(executor))
		, max_concurrent_(max_concurrent) {}

	job_id start(job_start spec) override {
		if (controllers_ == 0)
			throw std::runtime_error("background jobs unavailable: no job controller is attached (load tool-jobs)");
		if (spec.kind.empty())
			throw std::runtime_error("invalid job kind: expected a non-empty string");
		if (spec.label.empty())
			throw std::runtime_error("invalid job label: expected a non-empty string");
		if (spec.output_limit_bytes && *spec.output_limit_bytes == 0)
			throw std::runtime_error("invalid output_limit_bytes: expected a positive value");
		if (active_count(spec.owner_session) >= max_concurrent_)
			throw std::runtime_error(
				"background job limit reached for this owner (limit: " + std::to_string(max_concurrent_) +
				"); stop an unneeded job and retry");

		auto job = std::make_shared<tracked_job>();
		job->kind = spec.kind;
		job->label = spec.label;
		job->output_limit_bytes = spec.output_limit_bytes;
		job->owner_session = spec.owner_session;
		job->started_at_ms = now_ms();
		job->id = spec.kind + "-" + std::to_string(++counters_[spec.kind]);
		job->in_store = true;
		store_.emplace(job->id, job);

		std::weak_ptr<job_registry> weak_registry = weak_from_this();
		std::weak_ptr<tracked_job> weak_job = job;
		auto settle = [weak_registry, weak_job](job_outcome outcome) {
			if (auto registry = weak_registry.lock()) {
				if (auto tracked = weak_job.lock())
					registry->settle_job(std::move(tracked), std::move(outcome));
			}
		};

		try {
			auto handle = spec.run(std::move(settle));
			job->cancel = std::move(handle.cancel);
			job->read_output = std::move(handle.read_output);
		} catch (...) {
			store_.erase(job->id);
			job->in_store = false;
			notify_changed(job->owner_session);
			throw;
		}
		notify_changed(job->owner_session);
		return job->id;
	}

	std::vector<job_snapshot> list(std::optional<std::string> const& owner_session) const override {
		std::vector<job_snapshot> out;
		out.reserve(store_.size());
		for (auto const& [id, job] : store_) {
			if (!job->owner_session || (owner_session && *owner_session == *job->owner_session))
				out.push_back(snapshot(*job));
		}
		return out;
	}

	job_snapshot get(job_id const& id, std::optional<std::string> const& owner_session) const override {
		auto job = expect(id);
		check_access(*job, owner_session);
		return snapshot(*job);
	}

	job_read read(job_id const& id, std::optional<std::string> const& owner_session) override {
		auto job = expect(id);
		check_access(*job, owner_session);
		std::string text;
		if (job->read_output)
			text = job->read_output();
		else if (is_terminal(job->status))
			text = job->output;
		if (is_terminal(job->status))
			job->reported = true;
		return job_read{std::move(text), snapshot(*job)};
	}

	kill_result
	kill(job_id const& id, std::optional<std::string> const& owner_session, std::string const& reason) override {
		auto job = expect(id);
		check_access(*job, owner_session);
		if (is_terminal(job->status)) {
			job->reported = true;
			return kill_result::already_finished;
		}
		// Cancel first so a throw leaves lifecycle and notice state unchanged.
		if (job->cancel)
			job->cancel(reason);
		job->status = job_status::stopping;
		job->reported = true;
		notify_changed(job->owner_session);
		return kill_result::requested;
	}

	araya::task<job_snapshot>
	wait(job_id id, std::uint64_t timeout_ms, std::optional<std::string> const& owner_session, std::stop_token stop)
		override {
		auto job = expect(id);
		check_access(*job, owner_session);
		if (timeout_ms == 0)
			throw std::runtime_error("invalid wait timeout: expected a positive number of milliseconds");
		if (!is_terminal(job->status)) {
			auto token = std::make_shared<wait_token>();
			token->timer = std::make_shared<boost::asio::steady_timer>(executor_);
			token->timer->expires_after(std::chrono::milliseconds(timeout_ms));
			job->waiters.push_back(token);

			std::weak_ptr<tracked_job> weak_job = job;
			std::stop_callback stop_callback(stop, [token, ex = executor_] {
				boost::asio::post(ex, [token] {
					if (!token->active)
						return;
					token->active = false;
					token->timer->cancel();
				});
			});

			boost::system::error_code ec;
			co_await token->timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
			token->active = false;
			if (auto tracked = weak_job.lock()) {
				auto& waiters = tracked->waiters;
				waiters.erase(std::remove(waiters.begin(), waiters.end(), token), waiters.end());
			}
		}
		if (is_terminal(job->status))
			job->reported = true;
		co_return snapshot(*job);
	}

	araya::registration on_job_done(araya::plugin_context& caller, job_done_listener listener) override {
		auto token = next_listener_++;
		listeners_.emplace(token, std::move(listener));
		std::weak_ptr<job_registry> weak = weak_from_this();
		return caller.effect([weak, token]() -> araya::cleanup_action {
			return [weak, token] {
				if (auto registry = weak.lock())
					registry->listeners_.erase(token);
			};
		});
	}

	araya::registration on_jobs_changed(araya::plugin_context& caller, jobs_changed_listener listener) override {
		auto token = next_listener_++;
		changed_.emplace(token, std::move(listener));
		std::weak_ptr<job_registry> weak = weak_from_this();
		return caller.effect([weak, token]() -> araya::cleanup_action {
			return [weak, token] {
				if (auto registry = weak.lock())
					registry->changed_.erase(token);
			};
		});
	}

	araya::registration attach_controller(araya::plugin_context& caller, std::string name) override {
		(void)name;
		++controllers_;
		std::weak_ptr<job_registry> weak = weak_from_this();
		return caller.effect([weak]() -> araya::cleanup_action {
			return [weak] {
				if (auto registry = weak.lock()) {
					if (registry->controllers_ > 0)
						--registry->controllers_;
				}
			};
		});
	}

	// Owner cleanup: cancel and drop every job owned by a disposed session. The
	// producer may settle later; the record is already out of the store, so
	// nothing observable happens.
	void dispose_owner(std::string const& session) {
		bool removed = false;
		for (auto it = store_.begin(); it != store_.end();) {
			auto const& job = it->second;
			if (!job->owner_session || *job->owner_session != session) {
				++it;
				continue;
			}
			if (!is_terminal(job->status)) {
				job->reported = true;
				job->in_store = false;
				try {
					if (job->cancel)
						job->cancel("owner disposed");
				} catch (...) {
				}
				job->status = job_status::stopping;
				release_waiters(*job);
			}
			it = store_.erase(it);
			removed = true;
		}
		if (removed)
			notify_changed(std::optional<std::string>{session});
	}

	// Registry teardown: cancel live work and drop every record.
	void shutdown() {
		listeners_closed_ = true;
		for (auto& [id, job] : store_) {
			if (is_terminal(job->status))
				continue;
			job->reported = true;
			job->in_store = false;
			try {
				if (job->cancel)
					job->cancel("jobs service disposed");
			} catch (...) {
			}
			job->status = job_status::stopping;
			release_waiters(*job);
		}
		store_.clear();
	}

private:
	std::shared_ptr<tracked_job> expect(job_id const& id) const {
		auto found = store_.find(id);
		if (found == store_.end())
			throw std::runtime_error("unknown job " + id);
		return found->second;
	}

	void check_access(tracked_job const& job, std::optional<std::string> const& owner_session) const {
		if (job.owner_session && (!owner_session || *owner_session != *job.owner_session))
			throw std::runtime_error("job " + job.id + " belongs to another session");
	}

	std::size_t active_count(std::optional<std::string> const& owner_session) const {
		std::size_t count = 0;
		for (auto const& [id, job] : store_) {
			if (job->owner_session == owner_session &&
				(job->status == job_status::running || job->status == job_status::stopping))
				++count;
		}
		return count;
	}

	job_snapshot snapshot(tracked_job const& job) const {
		job_snapshot out;
		out.id = job.id;
		out.kind = job.kind;
		out.label = job.label;
		out.output_limit_bytes = job.output_limit_bytes;
		out.owner_session = job.owner_session;
		out.status = job.status;
		out.detail = job.detail;
		out.started_at_ms = job.started_at_ms;
		out.finished_at_ms = job.finished_at_ms;
		out.reported = job.reported;
		return out;
	}

	void release_waiters(tracked_job& job) {
		auto waiters = std::move(job.waiters);
		job.waiters.clear();
		for (auto& token : waiters) {
			if (!token->active)
				continue;
			token->active = false;
			token->timer->cancel();
		}
	}

	// First-wins settlement: record the outcome, release waiters, then announce
	// completion last (a listener may append to the owner's session).
	void settle_job(std::shared_ptr<tracked_job> job, job_outcome outcome) {
		if (is_terminal(job->status))
			return;
		if (!is_terminal(outcome.status))
			outcome.status = job_status::failed;
		job->status = outcome.status;
		job->detail = std::move(outcome.detail);
		job->output = std::move(outcome.output);
		job->finished_at_ms = now_ms();
		if (!job->waiters.empty())
			job->reported = true;
		release_waiters(*job);
		if (!job->in_store)
			return;
		notify_changed(job->owner_session);
		if (listeners_closed_)
			return;
		auto settled = snapshot(*job);
		for (auto const& [token, listener] : listeners_) {
			try {
				listener(settled, job->owner_session);
			} catch (...) {
			}
		}
	}

	void notify_changed(std::optional<std::string> const& owner_session) {
		for (auto const& [token, listener] : changed_) {
			try {
				listener(owner_session);
			} catch (...) {
			}
		}
	}

	boost::asio::any_io_executor executor_;
	std::size_t max_concurrent_;
	std::size_t controllers_ = 0;
	bool listeners_closed_ = false;
	std::uint64_t next_listener_ = 1;
	std::map<job_id, std::shared_ptr<tracked_job>> store_;
	std::map<std::string, std::uint64_t> counters_;
	std::map<std::uint64_t, job_done_listener> listeners_;
	std::map<std::uint64_t, jobs_changed_listener> changed_;
};

std::unique_ptr<araya::plugin> make_jobs(araya::plugin_config const& config) {
	struct jobs_plugin : araya::plugin {
		explicit jobs_plugin(araya::plugin_config const& cfg) {
			araya::plugin_config_view const view(cfg);
			if (auto value = view.try_get(max_concurrent_key))
				max_concurrent = static_cast<std::size_t>(*value);
		}

		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto registry = std::make_shared<job_registry>(ctx.executor(), max_concurrent);
			ctx.provide(jobs_key, std::shared_ptr<jobs_service>(registry));
			ctx.on(araya::session::disposed_key, [registry](araya::session::session_disposed_msg const& msg) {
				registry->dispose_owner(msg.id.value);
			});
			ctx.effect([registry]() -> araya::cleanup_action { return [registry] { registry->shutdown(); }; });
			co_return;
		}

		std::size_t max_concurrent = 10;
	};
	return std::make_unique<jobs_plugin>(config);
}

static const araya::dependency_spec g_deps[]{
	{araya::service_id{"sessions", 1}, true, {}},
};
static const araya::provision_spec g_provs[]{{araya::service_id{"jobs", 1}}};
static const araya::plugin_descriptor g_descriptor{"jobs", g_deps, g_provs, &make_jobs};

} // namespace

char const* job_status_name(job_status status) noexcept {
	switch (status) {
	case job_status::running:
		return "running";
	case job_status::stopping:
		return "stopping";
	case job_status::completed:
		return "completed";
	case job_status::killed:
		return "killed";
	case job_status::failed:
		return "failed";
	}
	return "failed";
}

bool is_terminal(job_status status) noexcept {
	return status == job_status::completed || status == job_status::killed || status == job_status::failed;
}

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::jobs
