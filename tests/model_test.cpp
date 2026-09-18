#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace {

struct database {
	std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

// ---- component pool --------------------------------------------------------

struct provider_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		ctx.provide(db_key, std::make_shared<database>("p"));
		co_return;
	}
};

struct consumer_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<database>(db_key);
		co_return;
	}
};

struct optional_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.find<database>(db_key);
		co_return;
	}
};

struct self_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.find<database>(db_key);
		ctx.provide(db_key, std::make_shared<database>("self"));
		co_return;
	}
};

struct broken_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		(void)ctx.require<database>(db_key);
		throw std::runtime_error("boom");
		co_return;
	}
};

template <class P>
std::unique_ptr<araya::plugin> make(araya::plugin_config const&) {
	return std::make_unique<P>();
}

static constexpr std::span<araya::dependency_spec const> g_no_deps{};
static constexpr std::span<araya::provision_spec const> g_no_provs{};
static const araya::dependency_spec g_db_dep[]{{araya::service_id{"example.db", 1}, true}};
static const araya::dependency_spec g_db_optional[]{{araya::service_id{"example.db", 1}, false}};
static const araya::provision_spec g_db_prov[]{{araya::service_id{"example.db", 1}}};

static const araya::plugin_descriptor g_desc_P{"P", g_no_deps, g_db_prov, &make<provider_plugin>};
static const araya::plugin_descriptor g_desc_C{"C", g_db_dep, g_no_provs, &make<consumer_plugin>};
static const araya::plugin_descriptor g_desc_O{"O", g_db_optional, g_no_provs, &make<optional_plugin>};
static const araya::plugin_descriptor g_desc_S{"S", g_db_optional, g_db_prov, &make<self_plugin>};
static const araya::plugin_descriptor g_desc_B{"B", g_db_dep, g_no_provs, &make<broken_plugin>};

araya::plugin_descriptor const* descriptor_for(char c) {
	switch (c) {
	case 'P':
		return &g_desc_P;
	case 'C':
		return &g_desc_C;
	case 'O':
		return &g_desc_O;
	case 'S':
		return &g_desc_S;
	case 'B':
		return &g_desc_B;
	}
	return nullptr;
}

std::shared_ptr<araya::plugin_descriptor> shared_desc(char c) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(descriptor_for(c)), [](auto*) {});
}

// ---- model ----------------------------------------------------------

struct op {
	char kind; // 'M' mount, 'R' retire
	char component;
	int slot;
};

std::vector<std::vector<op>> sequences(int n) {
	std::vector<op> ops;
	for (char c : {'P', 'C', 'O', 'S', 'B'})
		for (int s : {0, 1})
			ops.push_back({'M', c, s});
	for (int s : {0, 1})
		ops.push_back({'R', '\0', s});

	std::vector<std::vector<op>> out{{}};
	for (int i = 0; i < n; ++i) {
		std::vector<std::vector<op>> next;
		next.reserve(out.size() * ops.size());
		for (auto seq : out)
			for (auto o : ops) {
				seq.push_back(o);
				next.push_back(std::move(seq));
			}
		out = std::move(next);
	}
	return out;
}

using config = std::map<int, char>;

config final_config(std::vector<op> const& seq) {
	config cfg;
	for (auto const& o : seq) {
		if (o.kind == 'M') {
			if (!cfg.contains(o.slot))
				cfg[o.slot] = o.component;
		} else {
			cfg.erase(o.slot);
		}
	}
	return cfg;
}

// The observable quiescent state, keyed by component identity rather than
// per-run fiber ids so that different schedules can be compared.
struct observable {
	std::map<int, std::pair<araya::fiber_state, bool>> fibers;
	std::string db_provider;

	friend bool operator==(observable const&, observable const&) = default;
};

struct seq_driver {
	std::vector<op> seq;
	std::shared_ptr<araya::runtime> rt;
	std::shared_ptr<observable> out = std::make_shared<observable>();

	araya::task<void> operator()() {
		std::map<int, araya::fiber_handle> slot;
		std::map<araya::fiber_id, char> id_to_component;
		for (auto const& o : seq) {
			if (o.kind == 'M') {
				if (slot.contains(o.slot))
					continue;
				auto h = co_await rt->mount(araya::component_spec{shared_desc(o.component), {}, nullptr, ""});
				slot[o.slot] = h;
				id_to_component[h.id()] = o.component;
			} else {
				auto it = slot.find(o.slot);
				if (it != slot.end()) {
					co_await rt->retire(it->second);
					slot.erase(it);
				}
			}
		}
		co_await rt->wait_idle();
		co_await rt->validate_invariants_async();
		for (auto const& [s, h] : slot)
			out->fibers[s] = {rt->state_of(h.id()), rt->error_of(h.id()) != nullptr};
		if (auto const* b = rt->root()->lookup(db_key.id)) {
			auto it = id_to_component.find(b->provider);
			if (it != id_to_component.end())
				out->db_provider = std::string(1, it->second);
		}
		co_return;
	}
};

struct fresh_driver {
	std::vector<std::pair<int, char>> mounts; // reversed order
	std::shared_ptr<araya::runtime> rt;
	std::shared_ptr<observable> out = std::make_shared<observable>();

	araya::task<void> operator()() {
		std::map<int, araya::fiber_handle> slot;
		std::map<araya::fiber_id, char> id_to_component;
		for (auto const& [s, c] : mounts) {
			auto h = co_await rt->mount(araya::component_spec{shared_desc(c), {}, nullptr, ""});
			slot[s] = h;
			id_to_component[h.id()] = c;
		}
		co_await rt->wait_idle();
		co_await rt->validate_invariants_async();
		for (auto const& [s, h] : slot)
			out->fibers[s] = {rt->state_of(h.id()), rt->error_of(h.id()) != nullptr};
		if (auto const* b = rt->root()->lookup(db_key.id)) {
			auto it = id_to_component.find(b->provider);
			if (it != id_to_component.end())
				out->db_provider = std::string(1, it->second);
		}
		co_return;
	}
};

template <class Driver>
struct ref_driver {
	std::shared_ptr<Driver> d;

	araya::task<void> operator()() { co_await (*d)(); }
};

template <class Driver>
void run_once(boost::asio::io_context& io, Driver d, observable& result) {
	auto holder = std::make_shared<Driver>(std::move(d));
	auto out = holder->out;
	boost::asio::co_spawn(io.get_executor(), ref_driver<Driver>{std::move(holder)}, boost::asio::detached);
	io.run();
	io.restart();
	result = *out;
}

} // namespace

TEST_CASE("all short operation sequences quiesce, validate, and are "
		  "confluent") {
	auto all = sequences(4);
	std::map<config, observable> by_config;

	for (auto const& seq : all) {
		boost::asio::io_context io;
		auto rt = std::make_shared<araya::runtime>(io.get_executor());
		auto cfg = final_config(seq);

		observable ob;
		run_once(io, seq_driver{seq, rt}, ob);

		auto [it, inserted] = by_config.emplace(std::move(cfg), ob);
		if (!inserted)
			// Confluence (Theorem 80): the quiescent state is a function of
			// the final configuration alone, not of the schedule.
			REQUIRE(it->second == ob);
	}

	// Fresh-load agreement: a from-scratch mount of the final configuration
	// in a fixed, adversarially reversed order reaches the same state.
	for (auto const& [cfg, ob] : by_config) {
		boost::asio::io_context io;
		auto rt = std::make_shared<araya::runtime>(io.get_executor());

		std::vector<std::pair<int, char>> mounts(cfg.begin(), cfg.end());
		std::reverse(mounts.begin(), mounts.end());

		observable fresh;
		run_once(io, fresh_driver{std::move(mounts), rt}, fresh);
		REQUIRE(fresh == ob);
	}

	SUCCEED("enumerated " << all.size() << " sequences over " << by_config.size() << " configurations");
}
