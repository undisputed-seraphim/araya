#include <catch2/catch_test_macros.hpp>

#include "araya/agent-instructions/agent_instructions.hpp"
#include "araya/agent-loop/agent.hpp"
#include "araya/llm-mock/mock.hpp"
#include "araya/llm/bridge.hpp"
#include "araya/llm/llm.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/session/store.hpp"
#include "araya/system-prompt/system_prompt.hpp"
#include "araya/tools/tools.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

struct harness : araya_test::plugin_harness {
	araya::component_spec session_spec() { return spec(&araya::session::plugin_descriptor()); }
	araya::component_spec llm_spec() { return spec(&araya::llm::plugin_descriptor()); }
	araya::component_spec mock_spec() {
		return spec(&araya::llm_mock::plugin_descriptor(), {{"provider", "mock"}, {"response", "echo: {user}"}});
	}
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
	araya::component_spec tools_spec() { return spec(&araya::tools::plugin_descriptor()); }
	araya::component_spec agent_spec() { return spec(&araya::agent::plugin_descriptor()); }
	araya::component_spec instructions_spec(araya::plugin_config cfg = {}) {
		return spec(&araya::agent_instructions::plugin_descriptor(), std::move(cfg));
	}
};

// A scratch directory that removes itself.
struct scratch {
	fs::path dir;

	scratch() {
		dir = fs::temp_directory_path() / ("araya-instructions-" + std::to_string(::getpid()) + "-" +
										   std::to_string(reinterpret_cast<std::uintptr_t>(this)));
		fs::remove_all(dir);
		fs::create_directories(dir);
	}
	~scratch() {
		std::error_code ec;
		fs::remove_all(dir, ec);
	}
};

void write_file(fs::path const& path, std::string const& content) {
	fs::create_directories(path.parent_path());
	std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
}

// Drives one real turn so the context producer runs, then returns the text
// of the last context message this producer contributed.
struct rig {
	std::shared_ptr<araya::agent::agent_service> agent;
	std::shared_ptr<araya::session::session> session;
	std::string id = "s1";

	araya::task<void> mount(araya::runtime& rt, harness& h, fs::path const& cwd, araya::plugin_config cfg = {}) {
		co_await rt.mount(h.session_spec());
		co_await rt.mount(h.llm_spec());
		co_await rt.mount(h.mock_spec());
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.tools_spec());
		co_await rt.mount(h.agent_spec());
		co_await rt.mount(h.instructions_spec(std::move(cfg)));
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto store = root.require<araya::session::session_store>(araya::session::sessions_key).shared();
		agent = root.require<araya::agent::agent_service>(araya::agent::agent_key).shared();
		araya::session::create_session_options options;
		options.cwd = cwd.string();
		session = store->create(root, araya::session::session_id{id}, options);
	}

	araya::task<std::optional<std::string>> produced(std::string producer, std::string input = "go") {
		araya::agent::run_options options;
		options.provider = "mock";
		options.model = "mock-model";
		options.session = araya::session::session_id{id};
		options.input = std::move(input);
		co_await agent->run(options);
		auto const& messages = session->surface().messages();
		for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
			if (it->role != araya::session::message_role::user || !it->source)
				continue;
			auto const* object = it->source->if_object();
			auto const* kind = object ? object->if_contains("kind") : nullptr;
			auto const* plugin = object ? object->if_contains("plugin") : nullptr;
			if (!kind || !kind->is_string() || kind->as_string() != "plugin")
				continue;
			if (!plugin || !plugin->is_string() || plugin->as_string() != producer)
				continue;
			co_return araya::llm_bridge::message_text(*it);
		}
		co_return std::nullopt;
	}

	std::size_t produced_count(std::string_view producer) const {
		std::size_t count = 0;
		for (auto const& message : session->surface().messages()) {
			if (message.role != araya::session::message_role::user || !message.source)
				continue;
			auto const* object = message.source->if_object();
			auto const* plugin = object ? object->if_contains("plugin") : nullptr;
			if (plugin && plugin->is_string() && plugin->as_string() == producer)
				++count;
		}
		return count;
	}
};

} // namespace

TEST_CASE("loads a root AGENTS.md into an injected context message") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", "root rule one\n");
		rig r;
		co_await r.mount(rt, h, s.dir);

		auto text = co_await r.produced("agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->find("<system-reminder>") != std::string::npos);
		CHECK(text->find("Instructions from: AGENTS.md") != std::string::npos);
		CHECK(text->find("root rule one") != std::string::npos);
		CHECK(r.produced_count("agent-instructions") == 1);
	});
}

TEST_CASE("walks up to the project root and orders broadest first") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", "root rules\n");
		write_file(s.dir / "pkg" / "AGENTS.md", "pkg rules\n");
		rig r;
		co_await r.mount(rt, h, s.dir / "pkg");

		auto text = co_await r.produced("agent-instructions");
		REQUIRE(text.has_value());
		auto const root_at = text->find("root rules");
		auto const pkg_at = text->find("pkg rules");
		CHECK(root_at != std::string::npos);
		CHECK(pkg_at != std::string::npos);
		CHECK(root_at < pkg_at);
	});
}

TEST_CASE("candidate and local overlays load, with content dedup") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "CLAUDE.md", "shared rule\n");
		write_file(s.dir / "AGENTS.local.md", "shared rule\n");
		write_file(s.dir / "AGENTS.md", "primary rule\n");
		rig r;
		co_await r.mount(rt, h, s.dir);

		auto text = co_await r.produced("agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->find("primary rule") != std::string::npos);
		CHECK(text->find("shared rule") != std::string::npos);
		// Same-directory content dedup keeps the earliest candidate, so the
		// identical AGENTS.local.md overlay is dropped (CLAUDE.md survives).
		CHECK(text->find("Instructions from: AGENTS.md") != std::string::npos);
		CHECK(text->find("Instructions from: CLAUDE.md") != std::string::npos);
		CHECK(text->find("Instructions from: AGENTS.local.md") == std::string::npos);
	});
}

TEST_CASE("rendering stays within the byte budget") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", std::string(4096, 'x') + "\n");
		rig r;
		co_await r.mount(rt, h, s.dir, {{"max_bytes", "400"}});

		auto text = co_await r.produced("agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->size() <= 400);
		CHECK(text->find("Workspace instruction budget") != std::string::npos);
		CHECK(text->find("truncated") != std::string::npos);
	});
}

TEST_CASE("an empty workspace injects no context message") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		rig r;
		co_await r.mount(rt, h, s.dir);

		auto text = co_await r.produced("agent-instructions");
		CHECK_FALSE(text.has_value());
		CHECK(r.produced_count("agent-instructions") == 0);
	});
}

TEST_CASE("an unchanged workspace context is not re-injected across turns") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", "steady rule\n");
		rig r;
		co_await r.mount(rt, h, s.dir);

		co_await r.produced("agent-instructions", "first");
		co_await r.produced("agent-instructions", "second");
		CHECK(r.produced_count("agent-instructions") == 1);
	});
}
