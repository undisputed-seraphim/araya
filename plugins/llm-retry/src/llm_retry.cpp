#include "araya/llm-retry/llm_retry.hpp"

#include "araya/agent-loop/events.hpp"
#include "araya/events.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin_context.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace araya::llm_retry {
namespace {

using araya::agent::request_error_msg;

// The built-in transient set eligible in normal mode.
constexpr std::string_view default_retryable[] = {
	"empty_response",
	"rate_limit",
	"server",
	"timeout",
	"transport",
};

bool eligible(llm::llm_error_code code, std::vector<std::string> const& codes) {
	char const* name = llm::llm_error::code_name(code);
	if (codes.empty()) {
		for (auto const& candidate : default_retryable) {
			if (candidate == name)
				return true;
		}
		return false;
	}
	for (auto const& candidate : codes) {
		if (candidate == name)
			return true;
	}
	return false;
}

std::chrono::milliseconds backoff_delay(llm::retry_policy const& policy, std::uint32_t attempt, std::mt19937& rng) {
	std::chrono::milliseconds delay = policy.initial_delay;
	for (std::uint32_t i = 0; i < attempt && delay < policy.max_delay; ++i)
		delay = std::min(std::chrono::milliseconds(delay.count() * 2), policy.max_delay);
	if (policy.jitter_ratio > 0.0) {
		std::uniform_real_distribution<double> distribution(-1.0, 1.0);
		double const factor = 1.0 + policy.jitter_ratio * distribution(rng);
		delay = std::chrono::milliseconds(static_cast<std::int64_t>(static_cast<double>(delay.count()) * factor));
	}
	delay = std::max(delay, std::chrono::milliseconds(1));
	return std::min(delay, policy.max_delay);
}

// The retry decision for one failure, or nullopt to delegate.
std::optional<std::chrono::milliseconds>
decide(request_error_msg const& message, llm::retry_policy const& policy, std::mt19937& rng) {
	if (message.stop.stop_requested())
		return std::nullopt;
	if (policy.mode == llm::retry_mode::normal) {
		if (message.attempt >= policy.max_retries || !eligible(message.failure.code, policy.retryable_codes))
			return std::nullopt;
	}
	if (message.failure.provider_retry_after && *message.failure.provider_retry_after > std::chrono::milliseconds(0) &&
		*message.failure.provider_retry_after <= policy.max_delay)
		return *message.failure.provider_retry_after;
	return backoff_delay(policy, message.attempt, rng);
}

std::unique_ptr<araya::plugin> make_llm_retry(araya::plugin_config const&) {
	struct llm_retry_plugin : araya::plugin {
		araya::task<void> apply(araya::plugin_context& ctx) override {
			auto llm = ctx.require<llm::llm_service>(llm::llm_key).shared();
			auto rng = std::make_shared<std::mt19937>(std::random_device{}());
			ctx.on(
				araya::agent::request_error_key,
				[llm, rng](request_error_msg const& message, araya::waterfall_continuation<request_error_msg> next)
					-> boost::asio::awaitable<request_error_msg> {
					auto const policy = llm->provider_retry_policy(message.provider).value_or(llm::retry_policy{});
					auto const delay = decide(message, policy, *rng);
					if (!delay.has_value())
						co_return co_await next(message);

					auto executor = co_await boost::asio::this_coro::executor;
					auto timer = std::make_shared<boost::asio::steady_timer>(executor);
					timer->expires_after(*delay);
					std::stop_callback cancel(
						message.stop, [executor, timer] { boost::asio::post(executor, [timer] { timer->cancel(); }); });
					boost::system::error_code ec;
					co_await timer->async_wait(boost::asio::redirect_error(boost::asio::use_awaitable, ec));
					if (message.stop.stop_requested())
						co_return co_await next(message);

					request_error_msg resolved = message;
					resolved.retry = true;
					resolved.delay = *delay;
					co_return resolved;
				});
			co_return;
		}
	};
	return std::make_unique<llm_retry_plugin>();
}

static const araya::dependency_spec g_deps[]{{araya::service_id{"llm", 1}, true, {}}};
static constexpr std::span<araya::provision_spec const> g_provs{};
static const araya::plugin_descriptor g_descriptor{"llm-retry", g_deps, g_provs, &make_llm_retry, {}};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::llm_retry
