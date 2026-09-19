#include "araya/persistence/persistence.hpp"

#include "araya/plugin_context.hpp"
#include "araya/session/events.hpp"
#include "araya/session/store.hpp"
#include "araya/task.hpp"

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>

// The persistence provider: constructs the JSONL backend and wires it to
// the session firehose. The listeners are tracked effects on this fiber,
// so unloading the plugin removes them with it; sessions created while
// the plugin is down are simply not persisted (the same coverage rule as
// the harness's write handles).

namespace araya::persistence {
namespace {

using araya::session::appended_key;
using araya::session::created_key;
using araya::session::disposed_key;
using araya::session::flush_key;

struct persistence_plugin : araya::plugin {
	explicit persistence_plugin(std::filesystem::path root)
		: root_(std::move(root)) {}

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto backend = make_jsonl_backend(root_);

		// The listeners copy the shared_ptr; the provision takes the
		// local by move only after every capture has its own reference.
		ctx.on(
			created_key, [backend](araya::session::session_created_msg const& m) { backend->attach(m.s->header()); });
		ctx.on(
			appended_key, [backend](araya::session::session_appended_msg const& m) { backend->append(m.id, m.event); });
		ctx.on(flush_key, [backend](araya::session::session_flush_msg const& m) { backend->flush(m.id); });
		ctx.on(disposed_key, [backend](araya::session::session_disposed_msg const& m) { backend->detach(m.id); });

		ctx.provide(persistence_key, std::move(backend));
		co_return;
	}

	std::filesystem::path root_;
};

std::unique_ptr<araya::plugin> make_persistence(araya::plugin_config const& config) {
	std::filesystem::path root = "araya-sessions";
	if (auto it = config.find("root"); it != config.end() && !it->second.empty())
		root = it->second;
	return std::make_unique<persistence_plugin>(std::move(root));
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static const araya::provision_spec g_persistence_prov[]{{araya::service_id{"session.persistence", 1}}};
static const araya::plugin_descriptor g_descriptor{"persistence", g_no_deps, g_persistence_prov, &make_persistence};

} // namespace

araya::plugin_descriptor const& plugin_descriptor() { return g_descriptor; }

} // namespace araya::persistence
