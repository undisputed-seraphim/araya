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
inline constexpr araya::service_key<database> aux_key{"example.aux", 1};

static std::vector<std::string> g_log;
static araya::plugin_descriptor* g_child_desc = nullptr;
static std::vector<araya::fiber_handle> g_child_handles;

struct provider_plugin : araya::plugin {
	std::string value;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto tag = "p:" + value;
		ctx.effect([tag]() -> araya::cleanup_action { return [tag] { g_log.push_back("p-cleanup:" + tag); }; });
		ctx.provide(db_key, std::make_shared<database>(value));
		g_log.push_back("p-active:" + value);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_provider(araya::plugin_config const& cfg) {
	auto p = std::make_unique<provider_plugin>();
	p->value = cfg.at("value");
	return p;
}

// A parent that requires db, mounts a child, and provides aux.
struct parent_impl : araya::plugin {
	std::string tag;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		auto db = ctx.require<database>(db_key);
		g_log.push_back("parent-active:" + db->name);
		ctx.provide(aux_key, std::make_shared<database>("aux"));
		araya::component_spec child_spec;
		child_spec.descriptor = std::shared_ptr<araya::plugin_descriptor>(g_child_desc, [](auto*) {});
		child_spec.config = {{"tag", tag}};
		auto child = co_await ctx.mount(std::move(child_spec));
		g_child_handles.push_back(child);
		auto cleanup_tag = tag;
		ctx.effect([cleanup_tag]() -> araya::cleanup_action {
			return [cleanup_tag] { g_log.push_back("parent-cleanup:" + cleanup_tag); };
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_parent_impl(araya::plugin_config const& cfg) {
	auto p = std::make_unique<parent_impl>();
	p->tag = cfg.at("tag");
	return p;
}

struct child_impl : araya::plugin {
	std::string tag;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		// The child declares nothing; it reads db through the parent's
		// committed view (Algorithm 6).
		auto db = ctx.require<database>(db_key);
		g_log.push_back("child-active:" + tag + ":" + db->name);
		auto cleanup_tag = tag;
		ctx.effect([cleanup_tag]() -> araya::cleanup_action {
			return [cleanup_tag] { g_log.push_back("child-cleanup:" + cleanup_tag); };
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_child_impl(araya::plugin_config const& cfg) {
	auto p = std::make_unique<child_impl>();
	p->tag = cfg.at("tag");
	return p;
}

struct broken_child : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<database>(db_key);
		throw std::runtime_error("child failed");
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_broken_child(araya::plugin_config const&) {
	return std::make_unique<broken_child>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{{araya::service_id{"example.db", 1}, true}};
static const araya::provision_spec g_db_prov[]{{araya::service_id{"example.db", 1}}};
static const araya::provision_spec g_aux_prov[]{{araya::service_id{"example.aux", 1}}};

static const araya::plugin_descriptor g_provider_desc{"provider", g_no_deps, g_db_prov, &make_provider};
static const araya::plugin_descriptor g_parent_desc{"parent", g_db_dep, g_aux_prov, &make_parent_impl};
static const araya::plugin_descriptor g_child_desc_v{"child", g_no_deps, g_no_provs, &make_child_impl};
static const araya::plugin_descriptor g_broken_child_desc{"broken-child", g_no_deps, g_no_provs, &make_broken_child};

struct harness {
	boost::asio::io_context io;
	std::shared_ptr<araya::runtime> rt = std::make_shared<araya::runtime>(io.get_executor());

	template <typename Fn>
	void run(Fn&& fn) {
		g_log.clear();
		g_child_handles.clear();
		g_child_desc = const_cast<araya::plugin_descriptor*>(&g_child_desc_v);
		struct driver {
			std::decay_t<Fn> fn;
			harness* self;
			araya::task<void> operator()() { co_await fn(*self->rt); }
		};
		boost::asio::co_spawn(io.get_executor(), driver{std::forward<Fn>(fn), this}, boost::asio::detached);
		io.run();
		io.restart();
		g_child_desc = nullptr;
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

TEST_CASE("a plugin mounts a child that reads through the parent's view") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto parent = co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();

		CHECK(rt.state_of(parent.id()) == araya::fiber_state::active);
		REQUIRE(rt.fiber_count() == 3);
		REQUIRE(g_child_handles.size() == 1);
	});
	CHECK(std::find(g_log.begin(), g_log.end(), "parent-active:p1") != g_log.end());
	CHECK(std::find(g_log.begin(), g_log.end(), "child-active:a:p1") != g_log.end());
}

TEST_CASE("retiring a parent cascades to its children") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto parent = co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();
		REQUIRE(rt.fiber_count() == 3);

		co_await rt.retire(parent);
		co_await rt.wait_idle();
		CHECK(rt.fiber_count() == 1);
	});
	// The parent does not wait for its children (O-Retire is unconditional,
	// p. 35); the cascade is one level at a time, so both cleanups run.
	auto child_cleanup = std::find(g_log.begin(), g_log.end(), "child-cleanup:a");
	auto parent_cleanup = std::find(g_log.begin(), g_log.end(), "parent-cleanup:a");
	REQUIRE(child_cleanup != g_log.end());
	REQUIRE(parent_cleanup != g_log.end());
}

TEST_CASE("provider retirement cascades through parents to children") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto p = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();
		REQUIRE(rt.fiber_count() == 3);

		co_await rt.retire(p);
		co_await rt.wait_idle();
		// The provider is erased, the child cascades with the parent, and
		// the parent record remains inactive (it is host-managed).
		CHECK(rt.fiber_count() == 1);
	});
	auto child_cleanup = std::find(g_log.begin(), g_log.end(), "child-cleanup:a");
	auto parent_cleanup = std::find(g_log.begin(), g_log.end(), "parent-cleanup:a");
	auto provider_cleanup = std::find(g_log.begin(), g_log.end(), "p-cleanup:p:p1");
	REQUIRE(child_cleanup != g_log.end());
	REQUIRE(parent_cleanup != g_log.end());
	REQUIRE(provider_cleanup != g_log.end());
	// The parent declares db, so the guard orders it ahead of the provider;
	// the child declares nothing (it reads through the parent's committed
	// view), so its order relative to the provider is unconstrained.
	CHECK(parent_cleanup < provider_cleanup);
}

TEST_CASE("a reactivated parent mounts a fresh child") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		auto p1 = co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto parent = co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();
		REQUIRE(rt.fiber_count() == 3);

		co_await rt.retire(p1);
		co_await rt.wait_idle();
		CHECK(rt.state_of(parent.id()) == araya::fiber_state::inactive);
		CHECK(rt.fiber_count() == 1);

		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p2"}}));
		co_await rt.wait_idle();
		CHECK(rt.state_of(parent.id()) == araya::fiber_state::active);
		REQUIRE(rt.fiber_count() == 3);
		REQUIRE(g_child_handles.size() == 2);
		CHECK(std::find(g_log.begin(), g_log.end(), "child-active:a:p2") != g_log.end());
	});
}

TEST_CASE("a child failure stays on the child and leaves the parent active") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		g_child_desc = const_cast<araya::plugin_descriptor*>(&g_broken_child_desc);
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto parent = co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();

		CHECK(rt.state_of(parent.id()) == araya::fiber_state::active);
		REQUIRE(rt.fiber_count() == 3);
		REQUIRE(g_child_handles.size() == 1);
		CHECK(rt.state_of(g_child_handles[0].id()) == araya::fiber_state::inactive);
		CHECK(rt.error_of(g_child_handles[0].id()) != nullptr);
	});
}

TEST_CASE("host retirement of a child makes the parent's inverse a no-op") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		co_await rt.mount(h.spec(&g_provider_desc, {{"value", "p1"}}));
		auto parent = co_await rt.mount(h.spec(&g_parent_desc, {{"tag", "a"}}));
		co_await rt.wait_idle();
		REQUIRE(rt.fiber_count() == 3);
		REQUIRE(g_child_handles.size() == 1);

		co_await rt.retire(g_child_handles[0]);
		co_await rt.wait_idle();
		CHECK(rt.fiber_count() == 2);

		co_await rt.retire(parent);
		co_await rt.wait_idle();
		CHECK(rt.fiber_count() == 1);
	});
}
