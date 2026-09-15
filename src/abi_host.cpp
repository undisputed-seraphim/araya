#include "medulla/abi_host.hpp"

#include "medulla/activation.hpp"
#include "medulla/context.hpp"
#include "medulla/detail/gate.hpp"
#include "medulla/events.hpp"
#include "medulla/plugin_context.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace medulla::abi {

struct abi_bridge {
    explicit abi_bridge(plugin_context c) : ctx(std::move(c)) {}
    plugin_context ctx;
    std::shared_ptr<activation> act;
    boost::asio::any_io_executor strand;
    std::vector<std::shared_ptr<registration>> handles;
    std::shared_ptr<detail::gate_impl> done = detail::make_gate(false);
    std::exception_ptr error;
};

static abi_bridge* fiber_bridge(medulla_fiber fiber) {
    return static_cast<abi_bridge*>(fiber);
}

static int host_require(medulla_fiber fiber, const char* name,
                        std::uint32_t version, void** out) {
    try {
        auto b = fiber_bridge(fiber)->ctx.find_binding(
            service_id{name, version});
        if (!b)
            return MEDULLA_ABI_UNRESOLVED;
        *out = b->value.get();
        return MEDULLA_ABI_OK;
    } catch (...) {
        return MEDULLA_ABI_ERROR;
    }
}

static int host_find(medulla_fiber fiber, const char* name,
                     std::uint32_t version, void** out) {
    try {
        auto b = fiber_bridge(fiber)->ctx.find_binding(
            service_id{name, version});
        *out = b ? b->value.get() : nullptr;
        return MEDULLA_ABI_OK;
    } catch (...) {
        return MEDULLA_ABI_ERROR;
    }
}

static int host_provide(medulla_fiber fiber, const char* name,
                        std::uint32_t version, void* value,
                        void (*destroy_value)(void*), medulla_handle* out) {
    try {
        auto* b = fiber_bridge(fiber);
        std::shared_ptr<void> owned(
            value, destroy_value ? destroy_value : +[](void*) {});
        auto reg = std::make_shared<registration>(b->ctx.provide_raw(
            service_id{name, version}, std::move(owned)));
        b->handles.push_back(reg);
        if (out)
            *out = reg.get();
        return MEDULLA_ABI_OK;
    } catch (...) {
        return MEDULLA_ABI_ERROR;
    }
}

static int host_effect(medulla_fiber fiber,
                       void (*setup)(void* data, medulla_cleanup_v1* cleanup),
                       void* data, medulla_handle* out) {
    try {
        auto* b = fiber_bridge(fiber);
        auto reg = std::make_shared<registration>(
            b->ctx.effect([setup, data]() -> cleanup_action {
                medulla_cleanup_v1 c{};
                if (setup)
                    setup(data, &c);
                if (!c.run)
                    return nullptr;
                return [run = c.run, d = c.data] { run(d); };
            }));
        b->handles.push_back(reg);
        if (out)
            *out = reg.get();
        return MEDULLA_ABI_OK;
    } catch (...) {
        return MEDULLA_ABI_ERROR;
    }
}

static int host_on(medulla_fiber fiber, const char* name,
                   std::uint32_t version, std::uint32_t mode,
                   void (*invoke)(void* data, const void* msg), void* data,
                   medulla_handle* out) {
    try {
        auto* b = fiber_bridge(fiber);
        if (mode == MEDULLA_MODE_WATERFALL)
            return MEDULLA_ABI_UNSUPPORTED;
        auto bus = b->act->bus;
        if (!bus)
            return MEDULLA_ABI_ERROR;
        auto id = service_id{name, version};
        auto token = bus->add_raw_listener(
            id, static_cast<dispatch_mode>(mode),
            [invoke, data](void const* msg) -> boost::asio::awaitable<void> {
                invoke(data, msg);
                co_return;
            },
            b->act->id);
        auto index = b->act->effects->add(
            [bus, key = owned_service_id(id), token] {
                bus->remove_listener(key, token);
            });
        auto reg = std::make_shared<registration>(b->act->effects, index);
        b->handles.push_back(reg);
        if (out)
            *out = reg.get();
        return MEDULLA_ABI_OK;
    } catch (...) {
        return MEDULLA_ABI_ERROR;
    }
}

static void host_release(medulla_handle handle) {
    if (handle)
        static_cast<registration*>(handle)->release();
}

static int host_stop_requested(medulla_fiber fiber) {
    return fiber_bridge(fiber)->act->stop_token().stop_requested() ? 1 : 0;
}

static void host_post(medulla_fiber fiber, void (*fn)(void* data),
                      void* data) {
    boost::asio::post(fiber_bridge(fiber)->strand, [fn, data] { fn(data); });
}

static const medulla_host_api_v1 g_host_api{
    MEDULLA_ABI_VERSION,
    &host_require,
    &host_find,
    &host_provide,
    &host_effect,
    &host_on,
    &host_release,
    &host_stop_requested,
    &host_post,
};

medulla_host_api_v1 const& host_api() noexcept {
    return g_host_api;
}

namespace {

class abi_plugin final : public plugin {
public:
    explicit abi_plugin(medulla_instance_v1* instance) : instance_(instance) {}

    ~abi_plugin() override {
        if (instance_ && instance_->destroy)
            instance_->destroy(instance_);
    }

    task<void> apply(plugin_context& ctx) override {
        auto b = std::make_shared<abi_bridge>(ctx);
        b->act = ctx.activation_ptr();
        b->strand = co_await boost::asio::this_coro::executor;
        medulla_fiber fiber = b.get();
        instance_->apply(instance_, fiber, &on_complete);
        co_await b->done->wait(boost::asio::use_awaitable);
        if (b->error)
            std::rethrow_exception(b->error);
    }

    bool reconfigure(plugin_config const& cfg) override {
        if (!instance_ || !instance_->reconfigure)
            return false;
        std::vector<medulla_config_pair> pairs;
        pairs.reserve(cfg.size());
        for (auto const& [k, v] : cfg)
            pairs.push_back({k.c_str(), v.c_str()});
        return instance_->reconfigure(instance_, pairs.data(), pairs.size()) !=
               0;
    }

private:
    static void on_complete(medulla_fiber fiber, int code,
                            const char* error) {
        auto* b = static_cast<abi_bridge*>(fiber);
        if (code != MEDULLA_ABI_OK) {
            b->error = std::make_exception_ptr(std::runtime_error(
                error ? error : "module activation failed"));
        }
        b->done->open();
    }

    medulla_instance_v1* instance_;
};

struct descriptor_holder {
    plugin_descriptor desc;
    std::vector<std::string> names;
    std::vector<dependency_spec> deps;
    std::vector<provision_spec> provs;
};

}  // namespace

std::shared_ptr<plugin_descriptor> wrap(medulla_plugin_entry_fn entry,
                                        std::shared_ptr<void> keep_alive) {
    if (!entry)
        throw std::invalid_argument("null module entry point");
    const medulla_plugin_descriptor_v1* c = entry(&g_host_api);
    if (!c)
        throw std::runtime_error("module entry returned null descriptor");
    if (c->abi_version != MEDULLA_ABI_VERSION)
        throw std::runtime_error("module ABI version mismatch");

    auto holder = std::make_shared<descriptor_holder>();

    std::size_t name_count = 1 + c->inject_count + c->provide_count;
    holder->names.reserve(name_count);
    holder->names.push_back(c->name);
    for (std::size_t i = 0; i < c->inject_count; ++i)
        holder->names.push_back(c->inject[i].name);
    for (std::size_t i = 0; i < c->provide_count; ++i)
        holder->names.push_back(c->provide[i].name);

    holder->desc.name = holder->names[0];
    for (std::size_t i = 0; i < c->inject_count; ++i) {
        auto const& dep = c->inject[i];
        holder->deps.push_back(dependency_spec{
            service_id{holder->names[1 + i], dep.version},
            dep.required != 0, {}});
    }
    for (std::size_t i = 0; i < c->provide_count; ++i) {
        auto const& prov = c->provide[i];
        holder->provs.push_back(provision_spec{
            service_id{holder->names[1 + c->inject_count + i], prov.version}});
    }
    holder->desc.inject = holder->deps;
    holder->desc.provide = holder->provs;

    holder->desc.create =
        [c, keep_alive = std::move(keep_alive)](
            plugin_config const& cfg) -> std::unique_ptr<plugin> {
        std::vector<medulla_config_pair> pairs;
        pairs.reserve(cfg.size());
        for (auto const& [k, v] : cfg)
            pairs.push_back({k.c_str(), v.c_str()});
        medulla_instance_v1* instance = c->create(pairs.data(), pairs.size());
        if (!instance)
            return nullptr;
        return std::make_unique<abi_plugin>(instance);
    };

    return std::shared_ptr<plugin_descriptor>(holder, &holder->desc);
}

}  // namespace medulla::abi
