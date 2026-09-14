#include "medulla/detail/fiber.hpp"

#include <mutex>
#include <vector>

namespace medulla {
namespace detail {
namespace {

struct registry_entry {
    boost::asio::any_io_executor executor;
    std::weak_ptr<std::stop_source> stop_source;
};

std::mutex& registry_mutex() {
    static std::mutex m;
    return m;
}

std::vector<registry_entry>& registry() {
    static std::vector<registry_entry> r;
    return r;
}

}  // namespace

std::shared_ptr<fiber_control> make_fiber(
    boost::asio::any_io_executor const& ex) {
    auto ctl = std::make_shared<fiber_control>();
    ctl->id = next_fiber_id();
    ctl->cell = std::make_shared<fiber_cell>();
    ctl->cell->stop_source = std::make_shared<std::stop_source>();
    ctl->cell->signal = std::make_shared<boost::asio::cancellation_signal>();
    ctl->cell->state =
        std::make_shared<std::atomic<fiber_state>>(fiber_state::loading);
    {
        std::lock_guard lock{registry_mutex()};
        registry().push_back(registry_entry{ex, ctl->cell->stop_source});
    }
    return ctl;
}

fiber_id next_fiber_id() {
    static std::atomic<fiber_id> next_id{1};
    return next_id.fetch_add(1, std::memory_order_relaxed);
}

void retire_fiber(boost::asio::any_io_executor const& ex) {
    std::lock_guard lock{registry_mutex()};
    auto& r = registry();
    for (auto it = r.begin(); it != r.end(); ++it) {
        if (it->executor == ex) {
            r.erase(it);
            return;
        }
    }
}

void update_fiber_stop(boost::asio::any_io_executor const& ex,
                       std::shared_ptr<std::stop_source> stop_source) {
    std::lock_guard lock{registry_mutex()};
    for (auto& e : registry()) {
        if (e.executor == ex) {
            e.stop_source = std::move(stop_source);
            return;
        }
    }
}

std::shared_ptr<std::stop_source> fiber_registry_lookup(
    boost::asio::any_io_executor const& ex) {
    std::lock_guard lock{registry_mutex()};
    for (auto& e : registry()) {
        if (e.executor == ex)
            return e.stop_source.lock();
    }
    return nullptr;
}

}  // namespace detail
}  // namespace medulla
