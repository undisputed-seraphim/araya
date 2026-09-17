#include "araya/timer/timer.hpp"

#include <memory>
#include <span>
#include <stdexcept>

namespace araya::timer {
namespace {

// The timer provider: apply() constructs the service on the owning
// activation's bus executor and binds it under timer_key. No
// dependencies.
struct timer_plugin : araya::plugin {
    araya::task<void> apply(araya::plugin_context& ctx) override {
        if (!ctx.activation_ptr() || !ctx.activation_ptr()->bus)
            throw std::logic_error(
                "timer requires an activation with an event bus");
        auto service = std::make_shared<timer_service>(
            ctx.activation_ptr()->bus->executor());
        ctx.provide(timer_key, std::move(service));
        co_return;
    }
};

std::unique_ptr<araya::plugin> make_timer(
    araya::plugin_config const&) {
    return std::make_unique<timer_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_timer_prov[]{
    {araya::service_id{"timer", 1}}};
static const araya::plugin_descriptor g_descriptor{
    "timer", g_no_deps, g_timer_prov, &make_timer};

}  // namespace

araya::plugin_descriptor const& plugin_descriptor() {
    return g_descriptor;
}

}  // namespace araya::timer
