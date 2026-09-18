#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

struct database {
	std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

static std::vector<std::string> g_log;

struct provider_plugin : araya::plugin {
	std::string value;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>(value));
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_provider(araya::plugin_config const& cfg) {
	auto p = std::make_unique<provider_plugin>();
	p->value = cfg.at("value");
	return p;
}

struct broken_consumer : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("attempt");
		(void)ctx.require<database>(db_key);
		throw std::runtime_error("consumer always fails");
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_broken_consumer(araya::plugin_config const&) {
	return std::make_unique<broken_consumer>();
}

struct metadata_consumer : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto db = ctx.require<database>(db_key);
		for (auto const& [k, v] : db.metadata())
			g_log.push_back("meta:" + k + "=" + v);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_metadata_consumer(araya::plugin_config const&) {
	return std::make_unique<metadata_consumer>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{{araya::service_id{"example.db", 1}, true}};
static const araya::dependency_spec g_db_meta_dep[]{{araya::service_id{"example.db", 1}, true, {{"mode", "readonly"}}}};
static const araya::provision_spec g_db_prov[]{{araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_provider_desc{"provider", g_no_deps, g_db_prov, &make_provider};
static const araya::plugin_descriptor g_broken_consumer_desc{
	"broken-consumer",
	g_db_dep,
	g_no_provs,
	&make_broken_consumer};
static const araya::plugin_descriptor g_metadata_consumer_desc{
	"metadata-consumer",
	g_db_meta_dep,
	g_no_provs,
	&make_metadata_consumer};

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		g_log.clear();
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
	}

	araya::component_spec spec(araya::plugin_descriptor const* d, araya::plugin_config cfg = {}) {
		return araya::component_spec{
			std::shared_ptr<araya::plugin_descriptor>(const_cast<araya::plugin_descriptor*>(d), [](auto*) {}),
			std::move(cfg),
			nullptr,
			""};
	}
};

} // namespace

TEST_CASE("a raised fiber is not retried on dependency changes, only by "
		  "revision") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto consumer = co_await rt.mount(h.spec(&g_broken_consumer_desc));
		co_await rt.wait_idle();
		CHECK(rt.state_of(consumer.id()) == araya::fiber_state::inactive);
		CHECK(rt.error_of(consumer.id()) != nullptr);
		REQUIRE(std::count(g_log.begin(), g_log.end(), "attempt") == 1);

		// Environment change: the provider is replaced. The paper's failure
		// extension withholds re-entry, so the raised fiber stays down.
		auto p1 = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
		(void)p1;
		co_await rt.wait_idle();
		CHECK(rt.state_of(consumer.id()) == araya::fiber_state::inactive);
		CHECK(std::count(g_log.begin(), g_log.end(), "attempt") == 1);

		// A revision (reinsertion) clears the outcome and retries.
		co_await rt.retire(consumer);
		co_await rt.mount(h.spec(&g_broken_consumer_desc));
		co_await rt.wait_idle();
		CHECK(std::count(g_log.begin(), g_log.end(), "attempt") == 2);
	});
}

TEST_CASE("interception metadata merges at access with context priority") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		// The consumer declares mode=readonly; the context carries nothing
		// yet, so the lease carries the declaration.
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto consumer = co_await rt.mount(h.spec(&g_metadata_consumer_desc));
		co_await rt.wait_idle();
		CHECK(std::find(g_log.begin(), g_log.end(), "meta:mode=readonly") != g_log.end());

		// Context metadata now overrides and extends (Definition 27,
		// right-biased: the context takes priority).
		rt.root()->set_metadata(db_key.id, {{"mode", "readwrite"}, {"quota", "10"}});
		co_await rt.retire(consumer);
		co_await rt.mount(h.spec(&g_metadata_consumer_desc));
		co_await rt.wait_idle();
		CHECK(std::find(g_log.begin(), g_log.end(), "meta:mode=readwrite") != g_log.end());
		CHECK(std::find(g_log.begin(), g_log.end(), "meta:quota=10") != g_log.end());
	});
}
