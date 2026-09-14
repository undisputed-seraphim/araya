#pragma once

#include "medulla/service.hpp"
#include "medulla/task.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/experimental/promise.hpp>
#include <boost/asio/experimental/impl/promise.hpp>
#include <boost/asio/experimental/use_promise.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace medulla {

enum class dispatch_mode : std::uint8_t {
    emit,
    parallel,
    serial,
    waterfall,
};

template <class Message, dispatch_mode Mode>
struct event_key {
    service_id id;

    constexpr event_key(std::string_view name,
                        std::uint32_t version = 1) noexcept
        : id{name, version} {}

    constexpr event_key(service_id i) noexcept : id(i) {}
};

template <class Message>
class waterfall_continuation;

namespace detail {

template <class Message>
struct waterfall_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<Message>(
        Message const&, waterfall_continuation<Message>)>
        fn;
};

template <class Message>
struct notify_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<void>(Message const&)> fn;
};

template <class Message, class Fn>
auto make_notify_listener(Fn fn) {
    return [fn = std::move(fn)](
               Message const& m) -> boost::asio::awaitable<void> {
        using result_t = std::invoke_result_t<Fn const&, Message const&>;
        if constexpr (std::is_void_v<result_t>) {
            std::invoke(fn, m);
            co_return;
        } else {
            co_await std::invoke(fn, m);
        }
    };
}

template <class Message, class Fn>
auto make_waterfall_listener(Fn fn) {
    return [fn = std::move(fn)](
               Message const& m,
               waterfall_continuation<Message> next)
        -> boost::asio::awaitable<Message> {
        using result_t = std::invoke_result_t<
            Fn const&, Message const&, waterfall_continuation<Message>>;
        if constexpr (std::same_as<result_t, Message>) {
            co_return std::invoke(fn, m, std::move(next));
        } else {
            co_return co_await std::invoke(fn, m, std::move(next));
        }
    };
}

template <class Message, dispatch_mode Mode, class Fn>
auto make_listener(Fn&& fn) {
    if constexpr (Mode == dispatch_mode::waterfall)
        return make_waterfall_listener<Message>(std::forward<Fn>(fn));
    else
        return make_notify_listener<Message>(std::forward<Fn>(fn));
}

template <class Message, class Fn>
boost::asio::awaitable<void> invoke_owned(Fn const& fn, Message msg) {
    co_await fn(msg);
}

template <class Message, class Fn>
boost::asio::awaitable<void> invoke_owned_raw(Fn const& fn, Message msg) {
    co_await fn(&msg);
}

}  // namespace detail

template <class Message>
class waterfall_continuation {
public:
    waterfall_continuation(
        std::shared_ptr<std::vector<detail::waterfall_slot<Message>>> listeners,
        std::size_t index)
        : listeners_(std::move(listeners)), index_(index) {}

    boost::asio::awaitable<Message> operator()(Message msg) {
        if (index_ >= listeners_->size())
            co_return msg;
        co_return co_await (*listeners_)[index_].fn(
            msg, waterfall_continuation{listeners_, index_ + 1});
    }

private:
    std::shared_ptr<std::vector<detail::waterfall_slot<Message>>> listeners_;
    std::size_t index_ = 0;
};

namespace detail {

template <class Message>
struct raw_slot {
    std::uint64_t owner = 0;
    std::uint64_t token = 0;
    std::function<boost::asio::awaitable<void>(void const*)> fn;
};

struct event_entry_base {
    virtual ~event_entry_base() = default;
    virtual dispatch_mode mode() const noexcept = 0;
    virtual std::size_t count() const noexcept = 0;
    virtual void remove(std::uint64_t token) = 0;
    virtual void add_raw(std::uint64_t token, std::uint64_t owner,
                         std::function<boost::asio::awaitable<void>(void const*)> fn) = 0;
};

struct parallel_state {
    explicit parallel_state(std::size_t total,
                            boost::asio::any_io_executor strand)
        : remaining(total), gate(make_gate(*this, std::move(strand))) {}

    std::size_t remaining;
    std::exception_ptr first;
    std::move_only_function<void()> complete_handler;
    boost::asio::experimental::promise<void()> gate;

    void complete(std::exception_ptr ep) {
        if (ep && !first)
            first = ep;
        if (--remaining == 0) {
            auto h = std::move(complete_handler);
            h();
        }
    }

private:
    static boost::asio::experimental::promise<void()> make_gate(
        parallel_state& self, boost::asio::any_io_executor strand) {
        boost::asio::experimental::use_promise_t<> token;
        return boost::asio::async_initiate<
            boost::asio::experimental::use_promise_t<>, void()>(
            boost::asio::bind_executor(strand,
                                       [&self](auto&& handler) {
                                           self.complete_handler = handler;
                                       }),
            token);
    }
};

template <class Message, dispatch_mode Mode>
class event_entry_impl : public event_entry_base {
public:
    using slot_t = std::conditional_t<Mode == dispatch_mode::waterfall,
                                      waterfall_slot<Message>,
                                      notify_slot<Message>>;
    using snapshot_t = std::shared_ptr<std::vector<slot_t>>;

    dispatch_mode mode() const noexcept override { return Mode; }
    std::size_t count() const noexcept override {
        return listeners_->size() + raw_->size();
    }

    void add(slot_t slot) {
        auto updated = std::make_shared<std::vector<slot_t>>(*listeners_);
        updated->push_back(std::move(slot));
        listeners_ = std::move(updated);
    }

    void add_raw(std::uint64_t token, std::uint64_t owner,
                 std::function<boost::asio::awaitable<void>(void const*)> fn) override {
        auto updated = std::make_shared<std::vector<raw_slot<Message>>>(*raw_);
        updated->push_back(raw_slot<Message>{owner, token, std::move(fn)});
        raw_ = std::move(updated);
    }

    void remove(std::uint64_t token) override {
        auto updated = std::make_shared<std::vector<slot_t>>();
        updated->reserve(listeners_->size());
        for (auto& slot : *listeners_) {
            if (slot.token != token)
                updated->push_back(std::move(slot));
        }
        listeners_ = std::move(updated);

        auto raw_updated = std::make_shared<std::vector<raw_slot<Message>>>();
        raw_updated->reserve(raw_->size());
        for (auto& slot : *raw_) {
            if (slot.token != token)
                raw_updated->push_back(std::move(slot));
        }
        raw_ = std::move(raw_updated);
    }

    void dispatch_emit(
        Message msg, boost::asio::any_io_executor strand,
        std::shared_ptr<std::move_only_function<void(std::exception_ptr)>> sink) {
        auto snapshot = listeners_;
        for (auto const& slot : *snapshot) {
            Message m = msg;
            auto t = invoke_owned(slot.fn, std::move(m));
            boost::asio::co_spawn(
                strand, std::move(t),
                [sink](std::exception_ptr ep) {
                    if (ep && *sink)
                        (*sink)(ep);
                });
        }
        auto raw_snapshot = raw_;
        for (auto const& slot : *raw_snapshot) {
            Message m = msg;
            auto t = invoke_owned_raw(slot.fn, std::move(m));
            boost::asio::co_spawn(
                strand, std::move(t),
                [sink](std::exception_ptr ep) {
                    if (ep && *sink)
                        (*sink)(ep);
                });
        }
    }

    boost::asio::awaitable<void> dispatch_parallel(
        Message const& msg, boost::asio::any_io_executor strand) {
        auto snapshot = listeners_;
        if (snapshot->empty())
            co_return;
        auto raw_snapshot = raw_;
        auto state = std::make_shared<parallel_state>(
            snapshot->size() + raw_snapshot->size(), strand);
        for (auto const& slot : *snapshot) {
            auto t = slot.fn(msg);
            boost::asio::co_spawn(strand, std::move(t),
                                  [state](std::exception_ptr ep) {
                                      state->complete(ep);
                                  });
        }
        for (auto const& slot : *raw_snapshot) {
            auto t = slot.fn(&msg);
            boost::asio::co_spawn(strand, std::move(t),
                                  [state](std::exception_ptr ep) {
                                      state->complete(ep);
                                  });
        }
        co_await state->gate(boost::asio::use_awaitable);
        if (state->first)
            std::rethrow_exception(state->first);
    }

    boost::asio::awaitable<void> dispatch_serial(Message const& msg) {
        auto snapshot = listeners_;
        for (auto const& slot : *snapshot)
            co_await slot.fn(msg);
        auto raw_snapshot = raw_;
        for (auto const& slot : *raw_snapshot)
            co_await slot.fn(&msg);
    }

    boost::asio::awaitable<Message> dispatch_waterfall(Message const& msg) {
        auto snapshot = listeners_;
        co_return co_await waterfall_continuation<Message>{snapshot, 0}(msg);
    }

private:
    snapshot_t listeners_ = std::make_shared<std::vector<slot_t>>();
    std::shared_ptr<std::vector<raw_slot<Message>>> raw_ =
        std::make_shared<std::vector<raw_slot<Message>>>();
};

}  // namespace detail

class event_bus {
public:
    using strand_type = boost::asio::strand<boost::asio::any_io_executor>;

    explicit event_bus(strand_type control_strand);

    std::exception_ptr error;

    boost::asio::any_io_executor executor() const noexcept { return strand_; }

    void set_diagnostic_sink(
        std::move_only_function<void(std::exception_ptr)> sink);

    void report(std::exception_ptr ep);

    template <class Message, dispatch_mode Mode, class Fn>
    std::uint64_t add_listener(event_key<Message, Mode> const& key, Fn&& fn,
                               std::uint64_t owner) {
        ensure_on_strand();
        auto* base = entry_base_for(key.id);
        if (!base) {
            entries_.insert_or_assign(
                owned_service_id(key.id),
                std::make_unique<detail::event_entry_impl<Message, Mode>>());
            base = entry_base_for(key.id);
        } else if (base->mode() != Mode) {
            throw std::logic_error("listener registered with conflicting "
                                   "dispatch mode for event '" +
                                   std::string(key.id.name) + "'");
        }
        auto* impl = static_cast<detail::event_entry_impl<Message, Mode>*>(
            base);
        auto token = next_token_++;
        impl->add(typename detail::event_entry_impl<Message, Mode>::slot_t{
            owner, token, detail::make_listener<Message, Mode>(
                              std::forward<Fn>(fn))});
        return token;
    }

    void remove_listener(service_id id, std::uint64_t token);

    std::uint64_t add_raw_listener(
        service_id id, dispatch_mode mode,
        std::function<boost::asio::awaitable<void>(void const*)> fn,
        std::uint64_t owner);

    std::size_t listener_count(service_id id);

    template <class Message>
    void dispatch(event_key<Message, dispatch_mode::emit> const& key,
                  Message msg) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::emit>(key.id);
        if (!impl)
            return;
        impl->dispatch_emit(std::move(msg), strand_, sink_);
    }

    template <class Message>
    boost::asio::awaitable<void> dispatch(
        event_key<Message, dispatch_mode::parallel> const& key,
        Message const& msg) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::parallel>(key.id);
        if (!impl)
            co_return;
        co_await impl->dispatch_parallel(msg, strand_);
    }

    template <class Message>
    boost::asio::awaitable<void> dispatch(
        event_key<Message, dispatch_mode::serial> const& key,
        Message const& msg) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::serial>(key.id);
        if (!impl)
            co_return;
        co_await impl->dispatch_serial(msg);
    }

    template <class Message>
    boost::asio::awaitable<Message> dispatch(
        event_key<Message, dispatch_mode::waterfall> const& key,
        Message const& msg) {
        ensure_on_strand();
        auto* impl = typed_entry<Message, dispatch_mode::waterfall>(key.id);
        if (!impl)
            co_return msg;
        co_return co_await impl->dispatch_waterfall(msg);
    }

private:
    void ensure_on_strand() const;

    detail::event_entry_base* entry_base_for(service_id id) noexcept;

    template <class Message, dispatch_mode Mode>
    detail::event_entry_impl<Message, Mode>* typed_entry(service_id id) {
        auto* base = entry_base_for(id);
        if (!base)
            return nullptr;
        if (base->mode() != Mode)
            throw std::logic_error("dispatch mode mismatch for event '" +
                                   std::string(id.name) + "'");
        return static_cast<detail::event_entry_impl<Message, Mode>*>(base);
    }

    strand_type strand_;
    std::shared_ptr<std::move_only_function<void(std::exception_ptr)>> sink_ =
        std::make_shared<std::move_only_function<void(std::exception_ptr)>>();
    std::map<owned_service_id, std::unique_ptr<detail::event_entry_base>,
             transparent_id_less>
        entries_;
    std::uint64_t next_token_ = 1;
};

}  // namespace medulla
