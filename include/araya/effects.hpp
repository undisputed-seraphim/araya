#pragma once

#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <memory>

namespace araya {

using cleanup_action = std::move_only_function<void()>;

// The LIFO cleanup accumulator behind every activation. Cleanups are
// added the moment an effect succeeds (the paper's accumulator
// discipline) and run in reverse order at teardown. Owned by the
// activation; plugin code touches it only through `registration`.
class effect_stack : public std::enable_shared_from_this<effect_stack> {
public:
    std::exception_ptr error;

    // Appends a cleanup; returns the index registration uses to release
    // it. Not called by plugin code directly - use plugin_context::effect.
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

// An owned effect: one entry in the activation's cleanup stack.
//
// Lifecycle: return it, keep it, or drop it - the cleanup still runs at
// teardown in reverse registration order. release() runs it early
// (un-publishing the provision/listener it guards); releasing is
// idempotent. A default-constructed registration is inert (no stack).
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
