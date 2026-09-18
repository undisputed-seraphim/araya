#pragma once

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>

#include "araya/fiber_handle.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <stop_token>

namespace araya {
namespace detail {

struct fiber_control {
	fiber_id id;
	std::shared_ptr<fiber_cell> cell;
	// The terminal outcome lives in cell->error (published via
	// fiber_cell::publish_error), shared with the handle.

	fiber_handle to_handle() const {
		fiber_handle h;
		h.id_ = id;
		h.cell_ = cell;
		return h;
	}
};

std::shared_ptr<fiber_control> make_fiber(boost::asio::any_io_executor const& ex);

fiber_id next_fiber_id();

void retire_fiber(boost::asio::any_io_executor const& ex);

void update_fiber_stop(boost::asio::any_io_executor const& ex, std::shared_ptr<std::stop_source> stop_source);

std::shared_ptr<std::stop_source> fiber_registry_lookup(boost::asio::any_io_executor const& ex);

} // namespace detail
} // namespace araya
