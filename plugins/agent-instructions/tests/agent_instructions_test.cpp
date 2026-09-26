#include <catch2/catch_test_macros.hpp>

#include "araya/agent-instructions/agent_instructions.hpp"
#include "araya/plugin.hpp"
#include "araya/runtime.hpp"
#include "araya/system-prompt/system_prompt.hpp"

#include "support/plugin_harness.hpp"

#include <boost/asio/io_context.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>

#include <unistd.h>

namespace {

namespace fs = std::filesystem;

struct harness : araya_test::plugin_harness {
	araya::component_spec prompt_spec() {
		return spec(&araya::system_prompt::plugin_descriptor(), {{"include_harness_identity", "false"}});
	}
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

std::optional<std::string> section_of(araya::system_prompt::prompt_assembly const& assembly, std::string_view name) {
	for (auto const& section : assembly.sections) {
		if (section.name == name)
			return section.text;
	}
	return std::nullopt;
}

araya::system_prompt::prompt_assembly assemble(araya::plugin_context& root, fs::path const& cwd) {
	auto prompts =
		root.require<araya::system_prompt::system_prompt_service>(araya::system_prompt::system_prompt_key).shared();
	araya::system_prompt::assemble_context context;
	context.scope = "s1";
	context.cwd = cwd.string();
	return prompts->assemble(context);
}

} // namespace

TEST_CASE("loads a root AGENTS.md into the agent-instructions section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", "root rule one\n");
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.instructions_spec());
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto assembly = assemble(root, s.dir);
		auto text = section_of(assembly, "agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->find("<system-reminder>") != std::string::npos);
		CHECK(text->find("Instructions from: AGENTS.md") != std::string::npos);
		CHECK(text->find("root rule one") != std::string::npos);
	});
}

TEST_CASE("walks up to the project root and orders broadest first") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		write_file(s.dir / "AGENTS.md", "root rules\n");
		write_file(s.dir / "pkg" / "AGENTS.md", "pkg rules\n");
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.instructions_spec());
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto assembly = assemble(root, s.dir / "pkg");
		auto text = section_of(assembly, "agent-instructions");
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
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.instructions_spec());
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto assembly = assemble(root, s.dir);
		auto text = section_of(assembly, "agent-instructions");
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
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.instructions_spec({{"max_bytes", "400"}}));
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto assembly = assemble(root, s.dir);
		auto text = section_of(assembly, "agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->size() <= 400);
		CHECK(text->find("Workspace instruction budget") != std::string::npos);
		CHECK(text->find("truncated") != std::string::npos);
	});
}

TEST_CASE("an empty workspace renders no agent-instructions section") {
	harness h;
	h.run([&](araya::runtime& rt) -> araya::task<void> {
		scratch s;
		write_file(s.dir / ".git", "");
		co_await rt.mount(h.prompt_spec());
		co_await rt.mount(h.instructions_spec());
		co_await rt.wait_idle();

		auto root = rt.root_context();
		auto assembly = assemble(root, s.dir);
		auto text = section_of(assembly, "agent-instructions");
		REQUIRE(text.has_value());
		CHECK(text->empty());
	});
}
