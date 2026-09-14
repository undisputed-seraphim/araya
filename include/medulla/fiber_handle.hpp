#pragma once

#include <boost/asio/cancellation_signal.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <stop_token>

namespace medulla {

namespace detail {
struct fiber_control;
struct fiber_cell;
}

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
};

}  // namespace detail

class fiber_handle {
public:
    constexpr fiber_id id() const noexcept { return id_; }

    fiber_state state() const noexcept {
        return cell_ && cell_->state
                   ? cell_->state->load(std::memory_order_relaxed)
                   : fiber_state::inactive;
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

}  // namespace medulla
