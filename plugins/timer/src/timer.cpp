#include "araya/timer/timer.hpp"

#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

#include <memory>
#include <utility>

namespace araya::timer {

timer_service::timer_service(boost::asio::any_io_executor executor)
    : executor_(std::move(executor)) {}

araya::registration timer_service::timeout(
    plugin_context& caller, duration d, std::move_only_function<void()> fn) {
    auto timer = std::make_shared<boost::asio::steady_timer>(executor_);
    timer->expires_after(d);
    timer->async_wait([timer, fn = std::move(fn)](
                          boost::system::error_code ec) mutable {
        if (!ec)
            fn();
    });
    return caller.effect([timer]() -> araya::cleanup_action {
        return [timer] { timer->cancel(); };
    });
}

araya::registration timer_service::interval(
    plugin_context& caller, duration d, std::move_only_function<bool()> fn) {
    // The pending wait owns the state: while the timer is armed, the
    // completion handler keeps it alive; once it completes without
    // rescheduling (false return or cancellation) everything releases.
    struct interval_state : std::enable_shared_from_this<interval_state> {
        std::shared_ptr<boost::asio::steady_timer> timer;
        std::shared_ptr<std::move_only_function<bool()>> body;
        duration d;

        void schedule() {
            timer->expires_after(d);
            timer->async_wait(
                [self = shared_from_this()](boost::system::error_code ec) {
                    if (ec)
                        return;
                    if (!(*self->body)())
                        return;
                    self->schedule();
                });
        }
    };

    auto state = std::make_shared<interval_state>();
    state->timer = std::make_shared<boost::asio::steady_timer>(executor_);
    state->body = std::make_shared<std::move_only_function<bool()>>(
        std::move(fn));
    state->d = d;
    state->schedule();
    return caller.effect([timer = state->timer]() -> araya::cleanup_action {
        return [timer] { timer->cancel(); };
    });
}

araya::task<void> timer_service::sleep(duration d) {
    boost::asio::steady_timer timer{executor_};
    timer.expires_after(d);
    co_await timer.async_wait(boost::asio::use_awaitable);
}

}  // namespace araya::timer
