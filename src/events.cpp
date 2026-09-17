#include "araya/events.hpp"

namespace araya {

event_bus::event_bus(boost::asio::strand<boost::asio::any_io_executor>
                         control_strand)
    : strand_(std::move(control_strand)) {}

void event_bus::ensure_on_strand() const {
    if (!strand_.running_in_this_thread())
        throw std::logic_error(
            "event bus operations must run on the control strand");
}

detail::event_entry_base* event_bus::entry_base_for(
    service_id id) noexcept {
    auto found = entries_.find(id);
    return found != entries_.end() ? found->second.get() : nullptr;
}

void event_bus::remove_listener(service_id id, std::uint64_t token) {
    ensure_on_strand();
    if (auto* base = entry_base_for(id))
        base->remove(token);
}

std::uint64_t event_bus::add_raw_listener(
    service_id id, dispatch_mode mode,
    std::function<boost::asio::awaitable<void>(void const*)> fn,
    std::uint64_t owner) {
    ensure_on_strand();
    auto* base = entry_base_for(id);
    if (!base)
        throw std::logic_error("raw listener for undeclared event '" +
                               std::string(id.name) + "'");
    if (base->mode() != mode)
        throw std::logic_error("listener mode mismatch for event '" +
                               std::string(id.name) + "'");
    auto token = next_token_++;
    base->add_raw(token, owner, std::move(fn));
    return token;
}

std::size_t event_bus::listener_count(service_id id) {
    ensure_on_strand();
    auto found = entries_.find(id);
    return found != entries_.end() ? found->second->count() : 0;
}

void event_bus::set_diagnostic_sink(
    std::move_only_function<void(std::exception_ptr)> sink) {
    *sink_ = std::move(sink);
}

void event_bus::report(std::exception_ptr ep) {
    if (!error)
        error = ep;
    if (sink_ && *sink_)
        (*sink_)(ep);
}

}  // namespace araya
