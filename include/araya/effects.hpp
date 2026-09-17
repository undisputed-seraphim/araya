#pragma once

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>

namespace araya {

using cleanup_action = std::move_only_function<void()>;

class effect_stack : public std::enable_shared_from_this<effect_stack> {
public:
    std::exception_ptr error;

    std::size_t add(cleanup_action action);

    void run_entry(std::size_t index) noexcept;

    void run_all() noexcept;

    std::size_t size() const noexcept { return entries_.size(); }

private:
    struct entry {
        cleanup_action action;
        bool released = false;
    };

    std::deque<entry> entries_;
};

class registration {
public:
    registration() = default;

    explicit operator bool() const noexcept { return active_; }

    void release() noexcept;

    registration(std::shared_ptr<effect_stack> stack, std::size_t index)
        : stack_(std::move(stack)), index_(index), active_(true) {}

private:

    std::shared_ptr<effect_stack> stack_;
    std::size_t index_ = 0;
    bool active_ = false;
};

}  // namespace araya
