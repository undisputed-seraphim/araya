#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "oracle.hpp"

#include <boost/asio/detached.hpp>
#include <boost/asio/io_context.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace {

using araya_proof::fstate;
using araya_proof::machine;

struct database {
	std::string name;
};

inline constexpr araya::service_key<database> db_key{"example.db", 1};

static std::vector<std::string> g_log;

// ---- engine-side component pool (mirrors the oracle universe) -----------

template <char N>
struct logged : araya::plugin {
	static constexpr std::string name = std::string(1, N);

	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:" + name);
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:" + name); }; });
		co_return;
	}
};

struct provider_plugin : logged<'P'> {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:P");
		ctx.provide(db_key, std::make_shared<database>("p"));
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:P"); }; });
		co_return;
	}
};

struct reconfiguring_provider_plugin : logged<'Q'> {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:Q");
		ctx.provide(db_key, std::make_shared<database>("q"));
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:Q"); }; });
		co_return;
	}

	bool reconfigure(araya::plugin_config const&) override { return true; }
};

struct consumer_plugin : logged<'C'> {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:C");
		(void)ctx.require<database>(db_key);
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:C"); }; });
		co_return;
	}
};

struct optional_plugin : logged<'O'> {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:O");
		(void)ctx.find<database>(db_key);
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:O"); }; });
		co_return;
	}
};

struct self_plugin : logged<'S'> {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:S");
		(void)ctx.find<database>(db_key);
		ctx.provide(db_key, std::make_shared<database>("s"));
		(void)ctx.effect([]() -> araya::cleanup_action { return [] { g_log.push_back("cleanup:S"); }; });
		co_return;
	}
};

struct broken_plugin : araya::plugin {
	araya::task<void> apply(araya::plugin_context& ctx) override {
		g_log.push_back("apply:B");
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
static const araya::plugin_descriptor g_desc_Q{"Q", g_no_deps, g_db_prov, &make<reconfiguring_provider_plugin>};

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
	case 'Q':
		return &g_desc_Q;
	}
	return nullptr;
}

std::shared_ptr<araya::plugin_descriptor> shared_desc(char c) {
	return std::shared_ptr<araya::plugin_descriptor>(
		const_cast<araya::plugin_descriptor*>(descriptor_for(c)), [](auto*) {});
}

// ---- the oracle universe (the independent restatement) ------------------

std::map<char, araya_proof::component> oracle_pool() {
	std::map<char, araya_proof::component> pool;
	pool['P'] = {"P", {}, {"example.db"}};
	pool['C'] = {"C", {{"example.db", true}}, {}};
	pool['O'] = {"O", {{"example.db", false}}, {}};
	pool['S'] = {"S", {{"example.db", false}}, {"example.db"}};
	pool['B'] = {"B", {{"example.db", true}}, {}, true};
	pool['Q'] = {"Q", {}, {"example.db"}, false, true};
	return pool;
}

// ---- operations ----------------------------------------------------------

struct op {
	char kind;		// 'M' mount, 'R' retire, 'C' reconcile
	char component; // for 'M'/'C'
	int slot;		// 0 or 1
	char cfg;		// config value for 'M'/'C'
};

std::vector<std::vector<op>> sequences(int n) {
	std::vector<op> ops;
	for (char c : {'P', 'C', 'O', 'B', 'Q'})
		for (int s : {0, 1})
			ops.push_back({'M', c, s, '1'});
	// S (self-provider) is confined to slot 0: two same-key providers in
	// one scope is the paper's single-source violation, which the engine
	// diagnoses but permits — and then the two fibers invalidate each
	// other's bindings forever (no quiescence). The universe encodes the
	// discipline by construction.
	ops.push_back({'M', 'S', 0, '1'});
	for (int s : {0, 1})
		ops.push_back({'R', '\0', s, '0'});
	for (char c : {'P', 'Q'})
		for (int s : {0, 1})
			for (char cfg : {'1', '2'})
				ops.push_back({'C', c, s, cfg});

	std::vector<std::vector<op>> out{{}};
	for (int i = 0; i < n; ++i) {
		std::vector<std::vector<op>> next;
		next.reserve(out.size() * ops.size());
		for (auto seq : out) {
			for (auto o : ops) {
				seq.push_back(o);
				next.push_back(std::move(seq));
			}
		}
		out = std::move(next);
	}
	return out;
}

std::string to_string(op const& o) {
	std::string s;
	s += o.kind;
	if (o.kind != 'R')
		s += o.component;
	s += std::to_string(o.slot);
	if (o.kind == 'C')
		s += o.cfg;
	return s;
}

// ---- op interpretation (identical for both drivers) ----------------------
//
// Slot discipline: 'M' is valid only on an empty slot; 'R' only on an
// occupied slot; 'C' only on a slot whose occupant was reconciled (the
// engine's reconcile mounts a *new* fiber for a non-reconciled path, so
// 'C' on an imperatively mounted slot would fork the two models).

enum class slot_mode { empty, mounted, reconciled };

struct slot_view {
	slot_mode mode = slot_mode::empty;
	char comp = 0;
	char cfg = '1';
};

bool op_valid(op const& o, slot_view const& sv) {
	if (o.kind == 'M')
		return sv.mode == slot_mode::empty;
	if (o.kind == 'R')
		return sv.mode != slot_mode::empty;
	return sv.mode != slot_mode::mounted;
}

// ---- observables ---------------------------------------------------------

struct observable {
	std::map<int, std::pair<bool, bool>> fibers; // slot -> (alive?, error?)
	char db_provider = '\0';					 // component char bound to example.db
	std::vector<std::string> log;

	friend bool operator==(observable const&, observable const&) = default;
};

// ---- engine driver -------------------------------------------------------

struct engine_driver {
	std::vector<op> seq;
	std::shared_ptr<araya::runtime> rt;
	std::shared_ptr<observable> out = std::make_shared<observable>();

	araya::task<void> operator()() {
		std::map<int, araya::fiber_handle> slot;
		std::map<int, slot_view> sv;
		for (auto const& o : seq) {
			if (!op_valid(o, sv[o.slot]))
				continue;
			if (o.kind == 'M') {
				auto h = co_await rt->mount(
					araya::component_spec{shared_desc(o.component), {{"v", std::string(1, o.cfg)}}, nullptr, ""});
				slot[o.slot] = h;
				sv[o.slot] = {slot_mode::mounted, o.component, o.cfg};
			} else if (o.kind == 'R') {
				if (sv[o.slot].mode == slot_mode::mounted) {
					co_await rt->retire(slot[o.slot]);
				} else {
					// A reconciled slot is retired by omitting its path:
					// keep the other reconciled slots, retire this one.
					std::vector<araya::desired_component> keep;
					for (auto const& [s, v] : sv) {
						if (s != o.slot && v.mode == slot_mode::reconciled)
							keep.push_back(araya::desired_component{
								"s" + std::to_string(s),
								araya::component_spec{
									shared_desc(v.comp), {{"v", std::string(1, v.cfg)}}, nullptr, ""}});
					}
					co_await rt->reconcile(std::move(keep));
				}
				slot.erase(o.slot);
				sv[o.slot] = {};
			} else {
				sv[o.slot] = {slot_mode::reconciled, o.component, o.cfg};
				std::vector<araya::desired_component> desired;
				for (auto const& [s2, v] : sv)
					if (v.mode == slot_mode::reconciled)
						desired.push_back(araya::desired_component{
							"s" + std::to_string(s2),
							araya::component_spec{shared_desc(v.comp), {{"v", std::string(1, v.cfg)}}, nullptr, ""}});
				co_await rt->reconcile(std::move(desired));
				slot.erase(o.slot);
			}
			co_await rt->wait_idle();
		}
		co_await rt->wait_idle();
		co_await rt->validate_invariants_async();
		for (auto const& [s, h] : slot)
			out->fibers[s] = {true, rt->error_of(h.id()) != nullptr};
		// The bound value itself carries the provider's identity, which
		// is observable without fiber handles (reconciled fibers have none).
		if (auto const* b = rt->root()->lookup(db_key.id)) {
			if (b->value) {
				auto const* db = static_cast<database const*>(b->value.get());
				out->db_provider = std::toupper(db->name[0]);
			}
		}
		out->log = g_log;
		co_return;
	}
};

// ---- oracle driver -------------------------------------------------------

observable run_oracle(std::vector<op> const& seq) {
	machine m(oracle_pool());
	std::map<int, slot_view> sv;
	for (auto const& o : seq) {
		if (!op_valid(o, sv[o.slot]))
			continue;
		if (o.kind == 'M') {
			m.mount(o.slot, o.component, std::string(1, o.cfg));
			sv[o.slot] = {slot_mode::mounted, o.component, o.cfg};
		} else if (o.kind == 'R') {
			if (sv[o.slot].mode == slot_mode::reconciled) {
				std::map<int, std::pair<char, std::string>> desired;
				for (auto const& [s2, v] : sv)
					if (s2 != o.slot && v.mode == slot_mode::reconciled)
						desired[s2] = {v.comp, std::string(1, v.cfg)};
				m.reconcile_all(desired);
			} else {
				m.retire(o.slot);
			}
			sv[o.slot] = {};
		} else {
			sv[o.slot] = {slot_mode::reconciled, o.component, o.cfg};
			std::map<int, std::pair<char, std::string>> desired;
			for (auto const& [s2, v] : sv)
				if (v.mode == slot_mode::reconciled)
					desired[s2] = {v.comp, std::string(1, v.cfg)};
			m.reconcile_all(desired);
		}
	}
	observable ob;
	// Only imperatively mounted slots have engine-observable handles;
	// reconciled fibers are observed through bindings and the event log.
	for (auto const& [s, v] : sv)
		if (v.mode == slot_mode::mounted)
			ob.fibers[s] = {true, m.slot_errors[s]};
	ob.db_provider = m.db_provider;
	ob.log = std::move(m.log);
	return ob;
}

observable run_engine(std::vector<op> const& seq) {
	g_log.clear();
	boost::asio::io_context io;
	auto rt = std::make_shared<araya::runtime>(io.get_executor());
	auto d = std::make_shared<engine_driver>();
	d->seq = seq;
	d->rt = rt;
	auto out = d->out;
	struct holder {
		std::shared_ptr<engine_driver> d;
		araya::task<void> operator()() { co_await (*d)(); }
	};
	boost::asio::co_spawn(io.get_executor(), holder{d}, boost::asio::detached);
	io.run();
	return *out;
}

void check_conformance(std::vector<op> const& seq) {
	auto expected = run_oracle(seq);
	auto actual = run_engine(seq);
	std::string desc;
	for (auto const& o : seq) {
		desc += to_string(o);
		desc += ' ';
	}
	INFO("sequence: " << desc);
	CHECK(actual.fibers == expected.fibers);
	CHECK(actual.db_provider == expected.db_provider);
	CHECK(actual.log == expected.log);
}

} // namespace

TEST_CASE("the engine conforms to the paper oracle on every short "
		  "sequence") {
	auto all = sequences(3);
	for (auto const& seq : all)
		check_conformance(seq);
	SUCCEED("conformed on " << all.size() << " sequences");
}

TEST_CASE("the engine conforms to the paper oracle on mount/retire "
		  "sequences of length four") {
	std::vector<std::vector<op>> all{{}};
	std::vector<op> ops;
	for (char c : {'P', 'C', 'O', 'B', 'Q'})
		for (int s : {0, 1})
			ops.push_back({'M', c, s, '1'});
	// S (self-provider) is confined to slot 0: two same-key providers in
	// one scope is the paper's single-source violation, which the engine
	// diagnoses but permits — and then the two fibers invalidate each
	// other's bindings forever (no quiescence). The universe encodes the
	// discipline by construction.
	ops.push_back({'M', 'S', 0, '1'});
	for (int s : {0, 1})
		ops.push_back({'R', '\0', s, '0'});
	for (int i = 0; i < 4; ++i) {
		std::vector<std::vector<op>> next;
		next.reserve(all.size() * ops.size());
		for (auto seq : all)
			for (auto o : ops) {
				seq.push_back(o);
				next.push_back(std::move(seq));
			}
		all = std::move(next);
	}
	for (auto const& seq : all)
		check_conformance(seq);
	SUCCEED("conformed on " << all.size() << " sequences");
}

TEST_CASE("the engine conforms to the paper oracle under seeded chaos") {
	std::mt19937 rng(20260917);
	std::vector<op> ops;
	for (char c : {'P', 'C', 'O', 'B', 'Q'})
		for (int s : {0, 1})
			ops.push_back({'M', c, s, '1'});
	// S (self-provider) is confined to slot 0: two same-key providers in
	// one scope is the paper's single-source violation, which the engine
	// diagnoses but permits — and then the two fibers invalidate each
	// other's bindings forever (no quiescence). The universe encodes the
	// discipline by construction.
	ops.push_back({'M', 'S', 0, '1'});
	for (int s : {0, 1})
		ops.push_back({'R', '\0', s, '0'});
	for (char c : {'P', 'Q'})
		for (int s : {0, 1})
			for (char cfg : {'1', '2'})
				ops.push_back({'C', c, s, cfg});

	for (int iter = 0; iter < 40; ++iter) {
		std::vector<op> seq;
		for (int i = 0; i < 60; ++i)
			seq.push_back(ops[rng() % ops.size()]);
		check_conformance(seq);
	}
}
