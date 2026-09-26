#include <catch2/catch_test_macros.hpp>

#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/skill/skill.hpp"

#include "support/plugin_harness.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;

struct workspace {
	fs::path root;

	workspace() {
		std::error_code ec;
		auto const stamp = std::chrono::steady_clock::now().time_since_epoch().count();
		root = fs::temp_directory_path(ec) / ("araya-skill-" + std::to_string(stamp));
		fs::remove_all(root, ec);
		fs::create_directories(root / ".git", ec);
	}

	~workspace() {
		std::error_code ec;
		fs::remove_all(root, ec);
	}

	void write(fs::path const& relative, std::string const& content) {
		std::error_code ec;
		fs::create_directories((root / relative).parent_path(), ec);
		std::ofstream stream(root / relative, std::ios::binary);
		stream << content;
	}
};

struct harness : araya_test::plugin_harness {
	araya::component_spec skill_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::skill::plugin_descriptor(), std::move(cfg));
	}
};

} // namespace

TEST_CASE("skills are discovered as directory bundles and flat files") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		ws.write(
			".agents/skills/deep-dive/SKILL.md",
			"---\nname: deep-dive\ndescription: Investigate a topic thoroughly.\nwhenToUse: when the task "
			"needs research\n---\nStep one.\nStep two.\n");
		ws.write(
			".agents/skills/quick.md",
			"---\nname: quick\ndescription: A fast pass.\ndisable-model-invocation: true\n---\nFast body.\n");

		co_await rt.mount(h.skill_spec());
		co_await rt.wait_idle();
		auto skills = rt.root_context().require<araya::skill::skills_service>(araya::skill::skills_key).shared();

		auto list = skills->list(ws.root.string());
		REQUIRE(list.size() == 2);
		CHECK(list[0].name == "deep-dive");
		CHECK(list[0].description == "Investigate a topic thoroughly.");
		CHECK(list[0].when_to_use == std::optional<std::string>{"when the task needs research"});
		CHECK(list[0].source == "project-agents");
		CHECK(list[0].model_invocable);
		CHECK(list[1].name == "quick");
		CHECK_FALSE(list[1].model_invocable);
		CHECK(list[1].user_invocable);

		auto loaded = skills->get("deep-dive", ws.root.string());
		REQUIRE(loaded.has_value());
		CHECK(loaded->content == "Step one.\nStep two.");
		CHECK_FALSE(skills->get("missing", ws.root.string()).has_value());
	});
}

TEST_CASE("project-dsh outranks project-agents for a duplicate name") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		ws.write(".dsh/skills/dup/SKILL.md", "---\nname: dup\ndescription: the winner\n---\nwinner body\n");
		ws.write(".agents/skills/dup/SKILL.md", "---\nname: dup\ndescription: the loser\n---\nloser body\n");

		co_await rt.mount(h.skill_spec());
		co_await rt.wait_idle();
		auto skills = rt.root_context().require<araya::skill::skills_service>(araya::skill::skills_key).shared();

		auto list = skills->list(ws.root.string());
		REQUIRE(list.size() == 1);
		CHECK(list[0].description == "the winner");
		CHECK(list[0].source == "project-dsh");
		CHECK(skills->get("dup", ws.root.string())->content == "winner body");
	});
}

TEST_CASE("malformed skills are dropped") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		workspace ws;
		ws.write(".agents/skills/no-description/SKILL.md", "---\nname: no-description\n---\nbody\n");
		ws.write(
			".agents/skills/bad-boolean/SKILL.md",
			"---\nname: bad-boolean\ndescription: d\ndisable-model-invocation: perhaps\n---\nbody\n");
		ws.write(".agents/skills/Bad-Name/SKILL.md", "---\nname: Bad Name\ndescription: d\n---\nbody\n");
		ws.write(".agents/skills/ok/SKILL.md", "---\nname: ok\ndescription: d\n---\nbody\n");

		co_await rt.mount(h.skill_spec());
		co_await rt.wait_idle();
		auto skills = rt.root_context().require<araya::skill::skills_service>(araya::skill::skills_key).shared();

		auto list = skills->list(ws.root.string());
		REQUIRE(list.size() == 1);
		CHECK(list[0].name == "ok");
	});
}
