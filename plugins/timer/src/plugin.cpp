#include "medulla/timer/timer.hpp"

#include <memory>
#include <span>
#include <stdexcept>

namespace medulla::timer {
namespace {

// The timer provider: apply() constructs the service on the owning
// activation's bus executor and binds it under timer_key. No
// dependencies.
struct timer_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        if (!ctx.activation_ptr() || !ctx.activation_ptr()->bus)
            throw std::logic_error(
                "timer requires an activation with an event bus");
        auto service = std::make_shared<timer_service>(
            ctx.activation_ptr()->bus->executor());
        ctx.provide(timer_key, std::move(service));
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_timer(
    medulla::plugin_config const&) {
    return std::make_unique<timer_plugin>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static const medulla::provision_spec g_timer_prov[]{
    {medulla::service_id{"timer", 1}}};
static const medulla::plugin_descriptor g_descriptor{
    "timer", g_no_deps, g_timer_prov, &make_timer};

}  // namespace

medulla::plugin_descriptor const& plugin_descriptor() {
    return g_descriptor;
}

}  // namespace medulla::timer
