#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include "araya/detail/fiber.hpp"

#include <exception>
#include <memory>
#include <optional>
#include <stop_token>
#include <utility>

namespace araya {

// The coroutine type every plugin apply() and event listener returns.
// Executed on the strand it is co_spawned on; exceptions surface at the
// co_spawn caller (for the engine) or in the fiber record (for plugins).
template <typename T = void>
using task = boost::asio::awaitable<T, boost::asio::any_io_executor>;

// Spawns an ad-hoc fiber on a private strand and returns its handle.
//
// LIFETIME: independent of the runtime's fiber map - nothing to retire.
// It runs until the awaitable returns (state -> active), throws
// (state -> inactive; the exception is published to the handle's
// error() and stays readable after the task ends), or its handle's
// cancel() is called, which merely requests stop - watch
// this_stop_token()/stop_requested() inside the task. A task that exits
// cleanly after a stop request publishes no error.
template <typename Awaitable>
fiber_handle spawn(boost::asio::any_io_executor ex, Awaitable&& a) {
	auto strand = boost::asio::make_strand(std::move(ex));
	auto ctl = detail::make_fiber(strand);
	boost::asio::co_spawn(
		strand,
		std::forward<Awaitable>(a),
		boost::asio::bind_cancellation_slot(
			ctl->cell->signal->slot(), [ctl, strand](std::exception_ptr ep, auto&&...) mutable {
				ctl->cell->publish_error(ep);
				ctl->cell->state->store(
					(ep || ctl->cell->stop_source->stop_requested()) ? fiber_state::inactive : fiber_state::active,
					std::memory_order_relaxed);
				detail::retire_fiber(strand);
			}));
	return ctl->to_handle();
}

// The current fiber's stop token (ad-hoc fibers get theirs from spawn;
// plugin apply bodies get it from plugin_context::stop_token()). An
// empty token when not running inside a fiber.
inline boost::asio::awaitable<std::stop_token> this_stop_token() {
	auto ex = co_await boost::asio::this_coro::executor;
	auto src = detail::fiber_registry_lookup(ex);
	co_return src ? src->get_token() : std::stop_token{};
}

namespace detail {

struct stop_wait_state_base {
	virtual ~stop_wait_state_base() = default;
	virtual void complete() = 0;
};

template <typename Handler>
struct stop_wait_thunk;

template <typename Handler>
struct stop_wait_state : stop_wait_state_base, std::enable_shared_from_this<stop_wait_state<Handler>> {
	Handler handler;
	boost::asio::any_io_executor executor;
	std::stop_token token;
	std::optional<std::stop_callback<stop_wait_thunk<Handler>>> callback;

	stop_wait_state(Handler h, boost::asio::any_io_executor ex, std::stop_token tok)
		: handler(std::move(h))
		, executor(std::move(ex))
		, token(std::move(tok)) {}

	void arm() { callback.emplace(token, stop_wait_thunk<Handler>{this->shared_from_this()}); }

	void complete() override {
		callback.reset();
		Handler h = std::move(handler);
		h();
	}
};

template <typename Handler>
struct stop_wait_thunk {
	std::shared_ptr<stop_wait_state<Handler>> state;

	void operator()() const {
		boost::asio::post(state->executor, [state = state] { state->complete(); });
	}
};

struct stop_wait_initiation {
	std::stop_token token;

	template <typename Handler>
	void operator()(Handler&& handler) {
		if (token.stop_requested()) {
			std::move(handler)();
			return;
		}
		auto exec = boost::asio::get_associated_executor(handler);
		auto state = std::make_shared<stop_wait_state<std::decay_t<Handler>>>(
			std::forward<Handler>(handler), std::move(exec), token);
		state->arm();
	}
};

template <boost::asio::completion_token_for<void()> CompletionToken>
auto stop_wait(std::stop_token token, CompletionToken&& t) {
	return boost::asio::async_initiate<CompletionToken, void()>(stop_wait_initiation{std::move(token)}, t);
}

} // namespace detail

inline boost::asio::awaitable<bool> stop_requested() {
	auto ex = co_await boost::asio::this_coro::executor;
	auto src = detail::fiber_registry_lookup(ex);
	if (!src)
		co_return false;
	auto token = src->get_token();
	if (token.stop_requested())
		co_return true;
	co_await detail::stop_wait(token, boost::asio::use_awaitable);
	co_return true;
}

} // namespace araya
