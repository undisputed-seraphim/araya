#include "araya/session/store.hpp"

#include <memory>
#include <span>

namespace araya::session {
namespace {

// The sessions provider: apply() constructs the store and binds it under
// sessions_key. No dependencies — the store dispatches lifecycle events
// through the owning activation's event bus.
struct session_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto store = std::make_shared<session_store>(ctx);
		ctx.provide(sessions_key, std::move(store));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_session(araya::plugin_config const&) { return std::make_unique<session_plugin>(); }

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_sessions_prov[]{{araya::service_id{"sessions", 1}}};
static const araya::plugin_descriptor g_descriptor{"session", g_no_deps, g_sessions_prov, &make_session};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::session
