#pragma once

#include <boost/asio/cancellation_signal.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stop_token>

namespace araya {

namespace detail {
struct fiber_control;
struct fiber_cell;
} // namespace detail

// Where a fiber is in the lifecycle (the engine's theta). Loading covers
// apply() in progress; unloading covers teardown; a fiber that failed
// during loading returns to inactive.
enum class fiber_state : std::uint8_t {
	inactive,
	loading,
	active,
	unloading,
};

using fiber_id = std::uint64_t;

namespace detail {

struct fiber_cell {
	std::shared_ptr<std::stop_source> stop_source;
	std::shared_ptr<boost::asio::cancellation_signal> signal;
	std::shared_ptr<std::atomic<fiber_state>> state;
	// The fiber's terminal outcome, published lock-free: the writer (the
	// strand) stores error first and then release-publishes completed;
	// readers that observe completed acquire-load the error only then,
	// so no torn reads are possible. Written at most once per lifecycle
	// (reset only when a fiber re-enters loading).
	std::exception_ptr error;
	std::atomic<bool> completed{false};

	void publish_error(std::exception_ptr ep) noexcept {
		error = std::move(ep);
		completed.store(true, std::memory_order_release);
	}

	// Re-arm for a reactivation: clear the error before unpublishing.
	void reset_error() noexcept {
		error = nullptr;
		completed.store(false, std::memory_order_release);
	}
};

} // namespace detail

// A fiber handle: a cheap, copyable value that names one live activation.
// Handles do not keep the fiber alive - retire() the fiber and the
// handle's state() reads inactive from then on. cancel() is a
// cooperative stop request (request_stop only, never preemption); the
// fiber honors it through its stop_token.
//
// The terminal outcome is published into the shared cell (see above), so
// error() stays readable after the fiber's record is gone: for mounted
// fibers the runtime erases the record once the fiber finishes
// unloading, and for ad-hoc spawn()ed fibers there is no record at all.
class fiber_handle {
public:
	constexpr fiber_id id() const noexcept { return id_; }

	fiber_state state() const noexcept {
		return cell_ && cell_->state ? cell_->state->load(std::memory_order_relaxed) : fiber_state::inactive;
	}

	// The terminal failure, or nullptr. Nullptr means "no failure has
	// been recorded": the fiber is still running, finished cleanly, or
	// never had a cell. Safe from any thread - the completed flag's
	// release/acquire pair publishes the error slot.
	std::exception_ptr error() const noexcept {
		if (!cell_ || !cell_->completed.load(std::memory_order_acquire))
			return nullptr;
		return cell_->error;
	}

	void cancel() const noexcept {
		if (cell_ && cell_->stop_source)
			cell_->stop_source->request_stop();
	}

private:
	friend struct detail::fiber_control;

	fiber_id id_{};
	std::shared_ptr<detail::fiber_cell> cell_;
};

} // namespace araya
