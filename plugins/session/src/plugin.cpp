#include "medulla/session/store.hpp"

#include <memory>
#include <span>

namespace medulla::session {
namespace {

// The sessions provider: apply() constructs the store and binds it under
// sessions_key. No dependencies — the store dispatches lifecycle events
// through the owning activation's event bus.
struct session_plugin : medulla::plugin {
    medulla::task<void> apply(medulla::plugin_context& ctx) override {
        auto store = std::make_shared<session_store>(ctx);
        ctx.provide(sessions_key, std::move(store));
        co_return;
    }
};

std::unique_ptr<medulla::plugin> make_session(
    medulla::plugin_config const&) {
    return std::make_unique<session_plugin>();
}

static constexpr std::span<medulla::dependency_spec const> g_no_deps{};
static const medulla::provision_spec g_sessions_prov[]{
    {medulla::service_id{"sessions", 1}}};
static const medulla::plugin_descriptor g_descriptor{
    "session", g_no_deps, g_sessions_prov, &make_session};

}  // namespace

medulla::plugin_descriptor const& plugin_descriptor() {
    return g_descriptor;
}

}  // namespace medulla::session
