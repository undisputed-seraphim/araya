#include "medulla/effects.hpp"

#include "medulla/detail/assert.hpp"

namespace medulla {

std::size_t effect_stack::add(cleanup_action action) {
    MEDULLA_ASSERT(static_cast<bool>(action));
    entries_.push_back(entry{std::move(action), false});
    return entries_.size() - 1;
}

void effect_stack::run_entry(std::size_t index) noexcept {
    if (index >= entries_.size())
        return;
    auto& e = entries_[index];
    if (e.released)
        return;
    e.released = true;
    try {
        e.action();
    } catch (...) {
        if (!error)
            error = std::current_exception();
    }
}

void effect_stack::run_all() noexcept {
    std::size_t processed = 0;
    while (processed < entries_.size()) {
        run_entry(entries_.size() - 1 - processed);
        ++processed;
    }
    entries_.clear();
}

void registration::release() noexcept {
    if (!active_)
        return;
    active_ = false;
    stack_->run_entry(index_);
    stack_.reset();
}

}  // namespace medulla
