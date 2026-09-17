#pragma once

#include <boost/asio/async_result.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace araya {
namespace detail {

struct gate_impl : std::enable_shared_from_this<gate_impl> {
    explicit gate_impl(bool open) : open_(open) {}

    bool open_ = false;
    std::vector<std::move_only_function<void()>> waiters_;

    struct wait_op {
        std::shared_ptr<gate_impl> gate;

        template <typename Handler>
        void operator()(Handler&& handler) {
            if (gate->open_) {
                std::move(handler)();
                return;
            }
            gate->waiters_.push_back(std::move(handler));
        }
    };

    template <boost::asio::completion_token_for<void()> Token>
    auto wait(Token&& token) {
        return boost::asio::async_initiate<Token, void()>(
            wait_op{shared_from_this()}, token);
    }

    void open() {
        if (open_)
            return;
        open_ = true;
        auto waiters = std::move(waiters_);
        for (auto& w : waiters) {
            auto f = std::move(w);
            f();
        }
    }

    void close() { open_ = false; }
};

inline std::shared_ptr<gate_impl> make_gate(bool open = false) {
    return std::make_shared<gate_impl>(open);
}

}  // namespace detail
}  // namespace araya
