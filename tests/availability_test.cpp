#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct database {
	std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};
inline constexpr araya::event_key<std::string, araya::dispatch_mode::serial> promote_key{"example.promote", 1};

// The lazy provider: the availability check reads ready_ (false while
// "wait"), and a promote event calls set_available (AvailabilityFlip).
struct provider_plugin : araya::plugin {
	bool ready = false;

	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>("p"), [this] { return ready; });
		ctx.on(promote_key, [this, ctx](std::string const&) {
			ctx.set_available(db_key, true);
			ready = true;
		});
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_provider(araya::plugin_config const& cfg) {
	auto p = std::make_unique<provider_plugin>();
	if (auto it = cfg.find("start"); it != cfg.end() && it->second == "ready")
		p->ready = true;
	return p;
}

// The raising provider: check throws -> unavailable (never promotable).
struct throwing_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>("p"), []() -> bool { throw std::runtime_error("nope"); });
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_throwing(araya::plugin_config const&) {
	return std::make_unique<throwing_plugin>();
}

// Required consumer: parks until the provider promotes.
struct consumer_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<database>(db_key);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_consumer(araya::plugin_config const&) {
	return std::make_unique<consumer_plugin>();
}

// Optional consumer: applies with the empty view while unavailable and
// re-applies on promotion.
struct optional_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		++applies;
		(void)ctx.find<database>(db_key);
		co_return;
	}

	static inline int applies = 0;
};

std::unique_ptr<araya::plugin> make_optional(araya::plugin_config const&) {
	return std::make_unique<optional_plugin>();
}

// Impostor: a non-provider that tries to promote the binding.
struct impostor_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<database>(db_key);
		ctx.set_available(db_key, true);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_impostor(araya::plugin_config const&) {
	return std::make_unique<impostor_plugin>();
}

// Deactivator: a provider that tries to mark its own provision
// unavailable - rejected (deactivation is retirement).
struct deactivator_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>("p"));
		ctx.set_available(db_key, false);
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_deactivator(araya::plugin_config const&) {
	return std::make_unique<deactivator_plugin>();
}

// Counts how often the provider's check ran.
inline int g_checks = 0;

struct counting_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>("p"), []() -> bool {
			++g_checks;
			return false;
		});
		ctx.on(promote_key, [ctx](std::string const&) { ctx.set_available(db_key, true); });
		co_return;
	}
};

std::unique_ptr<araya::plugin> make_counting(araya::plugin_config const&) {
	return std::make_unique<counting_plugin>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{{araya::service_id{"example.db", 1}, true, {}}};
static const araya::dependency_spec g_db_opt[]{{araya::service_id{"example.db", 1}, false, {}}};
static const araya::provision_spec g_db_prov[]{{araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_desc_P{"P", g_no_deps, g_db_prov, &make_provider};
static const araya::plugin_descriptor g_desc_T{"T", g_no_deps, g_db_prov, &make_throwing};
static const araya::plugin_descriptor g_desc_N{"N", g_no_deps, g_db_prov, &make_counting};
static const araya::plugin_descriptor g_desc_C{"C", g_db_dep, g_no_provs, &make_consumer};
static const araya::plugin_descriptor g_desc_O{"O", g_db_opt, g_no_provs, &make_optional};
static const araya::plugin_descriptor g_desc_I{"I", g_db_dep, g_no_provs, &make_impostor};
static const araya::plugin_descriptor g_desc_D{"D", g_no_deps, g_db_prov, &make_deactivator};

std::shared_ptr<araya::plugin_descriptor> shared_desc(araya::plugin_descriptor const& d) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(&d), [](araya::plugin_descriptor*) {});
}

araya::component_spec spec(araya::plugin_descriptor const& d, araya::plugin_config cfg = {}) {
	return {shared_desc(d), std::move(cfg), nullptr, "", {}};
}

// Runs one coroutine body on the io_context and drains it.
void run(boost::asio::io_context& io, std::move_only_function<araya::task<void>()> body) {
	auto b = std::make_shared<std::move_only_function<araya::task<void>()>>(std::move(body));
	boost::asio::co_spawn(io.get_executor(), [b]() -> araya::task<void> { co_await (*b)(); }, boost::asio::detached);
	io.run();
	io.restart();
}

} // namespace

TEST_CASE("unavailable providers park consumers until promotion") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());

	araya::fiber_handle p, c;
	run(io, [&]() -> araya::task<void> {
		p = co_await rt->mount(spec(g_desc_P, {{"start", "wait"}}));
		co_await rt->wait_idle();
		c = co_await rt->mount(spec(g_desc_C));
		co_await rt->wait_idle();

		// Parked: the binding exists but is unavailable, so the required
		// consumer recorded its resolution error and stayed inactive.
		CHECK(rt->state_of(c.id()) == araya::fiber_state::inactive);
		CHECK(rt->error_of(c.id()) != nullptr);

		co_await rt->bus()->dispatch(promote_key, std::string("go"));
		co_await rt->wait_idle();

		CHECK(rt->state_of(c.id()) == araya::fiber_state::active);
		CHECK(rt->error_of(c.id()) == nullptr);
		co_await rt->validate_invariants_async();
	});
}

TEST_CASE("optional consumers proceed empty and re-apply on promotion") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());
	optional_plugin::applies = 0;

	run(io, [&]() -> araya::task<void> {
		co_await rt->mount(spec(g_desc_P, {{"start", "wait"}}));
		co_await rt->wait_idle();
		auto o = co_await rt->mount(spec(g_desc_O));
		co_await rt->wait_idle();

		// The optional consumer ran once with the empty view.
		CHECK(rt->state_of(o.id()) == araya::fiber_state::active);
		CHECK(optional_plugin::applies == 1);

		co_await rt->bus()->dispatch(promote_key, std::string("go"));
		co_await rt->wait_idle();

		// The binding "appeared": the optional consumer re-applies.
		CHECK(optional_plugin::applies == 2);
		auto db = rt->root()->lookup(db_key.id);
		REQUIRE(db != nullptr);
		co_await rt->validate_invariants_async();
	});
}

TEST_CASE("a throwing check reads as unavailable") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());

	run(io, [&]() -> araya::task<void> {
		co_await rt->mount(spec(g_desc_T));
		co_await rt->wait_idle();
		auto c = co_await rt->mount(spec(g_desc_C));
		co_await rt->wait_idle();

		CHECK(rt->state_of(c.id()) == araya::fiber_state::inactive);
		CHECK(rt->error_of(c.id()) != nullptr);
	});
}

TEST_CASE("deactivation is rejected and impostors cannot promote") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());

	bool saw_logic = false;
	run(io, [&]() -> araya::task<void> {
		// The deactivator tries set_available(false) in apply: the fiber
		// fails with the logic_error.
		auto d = co_await rt->mount(spec(g_desc_D));
		co_await rt->wait_idle();
		CHECK(rt->state_of(d.id()) == araya::fiber_state::inactive);
		REQUIRE(rt->error_of(d.id()) != nullptr);
		try {
			std::rethrow_exception(rt->error_of(d.id()));
		} catch (std::logic_error const&) {
			saw_logic = true;
		}
	});
	CHECK(saw_logic);
	saw_logic = false;

	run(io, [&]() -> araya::task<void> {
		co_await rt->mount(spec(g_desc_P, {{"start", "ready"}}));
		co_await rt->wait_idle();
		// Promote on the ready provider: fine (idempotent path).
		co_await rt->bus()->dispatch(promote_key, std::string("go"));
		co_await rt->wait_idle();
	});

	// The impostor: requires db, then tries to promote it -> logic_error,
	// so the fiber fails with that error.
	run(io, [&]() -> araya::task<void> {
		auto i = co_await rt->mount(spec(g_desc_I));
		co_await rt->wait_idle();
		CHECK(rt->state_of(i.id()) == araya::fiber_state::inactive);
		REQUIRE(rt->error_of(i.id()) != nullptr);
		try {
			std::rethrow_exception(rt->error_of(i.id()));
		} catch (std::logic_error const&) {
			saw_logic = true;
		}
	});
	CHECK(saw_logic);
}

TEST_CASE("the availability check runs once at provide-time") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());
	g_checks = 0;

	run(io, [&]() -> araya::task<void> {
		co_await rt->mount(spec(g_desc_N));
		co_await rt->wait_idle();
		auto c = co_await rt->mount(spec(g_desc_C));
		co_await rt->wait_idle();
		co_await rt->bus()->dispatch(promote_key, std::string("go"));
		co_await rt->wait_idle();

		CHECK(rt->state_of(c.id()) == araya::fiber_state::active);
		// Once at provide-time, not at every resolution or promotion.
		CHECK(g_checks == 1);
		co_await rt->validate_invariants_async();
	});
}

TEST_CASE("retirement still tears dependents down (deactivation path)") {
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());

	run(io, [&]() -> araya::task<void> {
		auto p = co_await rt->mount(spec(g_desc_P, {{"start", "ready"}}));
		co_await rt->wait_idle();
		auto c = co_await rt->mount(spec(g_desc_C));
		co_await rt->wait_idle();
		CHECK(rt->state_of(c.id()) == araya::fiber_state::active);

		// The only way back to unavailable: unload the provider.
		co_await rt->retire(p);
		co_await rt->wait_idle();
		CHECK(rt->state_of(c.id()) == araya::fiber_state::inactive);
		CHECK(rt->error_of(c.id()) != nullptr);
	});
}
