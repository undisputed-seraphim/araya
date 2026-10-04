#include <catch2/catch_test_macros.hpp>

#include "araya/config.hpp"
#include "araya/plugin.hpp"
#include "config.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Saves and restores one environment variable across a test case.
struct env_guard {
	std::string name;
	std::optional<std::string> old;

	explicit env_guard(char const* n)
		: name(n) {
		if (auto const* value = std::getenv(n))
			old = value;
	}

	~env_guard() {
		if (old)
			::setenv(name.c_str(), old->c_str(), 1);
		else
			::unsetenv(name.c_str());
	}
};

// A per-test working directory with the home/project layers silenced.
struct sandbox {
	fs::path dir;
	fs::path previous = fs::current_path();
	env_guard state{"ARAYA_STATE_DIR"};
	env_guard config_home{"XDG_CONFIG_HOME"};
	env_guard config{"ARAYA_CONFIG"};

	sandbox()
		: dir(fs::temp_directory_path() / fs::path("araya-config-test-" + std::to_string(::getpid()))) {
		fs::remove_all(dir);
		fs::create_directories(dir);
		::setenv("ARAYA_STATE_DIR", (dir / "state").c_str(), 1);
		::setenv("XDG_CONFIG_HOME", (dir / "config").c_str(), 1);
		::unsetenv("ARAYA_CONFIG");
		fs::current_path(dir);
	}

	~sandbox() {
		fs::current_path(previous);
		fs::remove_all(dir);
	}

	std::string write(std::string const& name, std::string const& contents) {
		auto path = dir / name;
		std::ofstream out(path);
		out << contents;
		return path.string();
	}
};

constexpr araya::config_key<std::uint64_t> timeout_key{"timeout_ms"};
constexpr araya::config_field k_shell_fields[] = {
	araya::field(timeout_key, "Per-command timeout in milliseconds.", "30000"),
};
constexpr std::span<araya::config_field const> k_shell_schema{k_shell_fields};
const araya::plugin_descriptor k_shell_desc{"shell", {}, {}, nullptr, k_shell_schema};

araya::plugin_descriptor const* stub_lookup(std::string_view id) { return id == "shell" ? &k_shell_desc : nullptr; }

} // namespace

TEST_CASE("resolve_app_config layers overlays per key, later winning") {
	sandbox box;
	auto first = box.write("first.json", R"({"components":{"shell":{"timeout_ms":"1000"}}})");
	auto second = box.write(
		"second.json", R"({"components":{"shell":{"timeout_ms":"2000"},"tool-web":{"search_max_results":"5"}}})");

	std::vector<araya::app::config_warning> warnings;
	auto config = araya::app::resolve_app_config({first, second}, {}, warnings);

	REQUIRE(config.components.count("shell") == 1);
	CHECK(config.components.at("shell").at("timeout_ms") == "2000");
	REQUIRE(config.components.count("tool-web") == 1);
	CHECK(config.components.at("tool-web").at("search_max_results") == "5");
	CHECK(config.layers.size() == 2);
	CHECK(warnings.empty());
}

TEST_CASE("environment and CLI outrank the config file") {
	sandbox box;
	env_guard level{"ARAYA_LOG_LEVEL"};
	auto file = box.write("cfg.json", R"({"log":{"level":"warn"}})");

	// File only.
	std::vector<araya::app::config_warning> warnings;
	auto from_file = araya::app::resolve_app_config({file}, {}, warnings);
	CHECK(from_file.log_level == "warn");

	// Environment beats the file.
	::setenv("ARAYA_LOG_LEVEL", "debug", 1);
	auto from_env = araya::app::resolve_app_config({file}, {}, warnings);
	CHECK(from_env.log_level == "debug");

	// CLI beats the environment.
	araya::app::config_cli cli;
	cli.log_level = "error";
	auto from_cli = araya::app::resolve_app_config({file}, cli, warnings);
	CHECK(from_cli.log_level == "error");
}

TEST_CASE("state paths default under the state base") {
	sandbox box;
	std::vector<araya::app::config_warning> warnings;
	auto config = araya::app::resolve_app_config({}, {}, warnings);
	auto base = box.dir / "state";
	CHECK(config.state_dir == base.string());
	CHECK(config.sessions_dir == (base / "sessions").string());
	CHECK(config.attachments_dir == (base / "attachments").string());
	CHECK(config.log_file == (base / "logs" / "araya.log").string());
}

TEST_CASE("a malformed overlay refuses to resolve") {
	sandbox box;
	auto bad = box.write("bad.json", "{ not json");
	std::vector<araya::app::config_warning> warnings;
	CHECK_THROWS_AS(araya::app::resolve_app_config({bad}, {}, warnings), std::runtime_error);
}

TEST_CASE("a missing explicit overlay refuses to resolve") {
	sandbox box;
	std::vector<araya::app::config_warning> warnings;
	CHECK_THROWS_AS(
		araya::app::resolve_app_config({(box.dir / "nope.json").string()}, {}, warnings), std::runtime_error);
}

TEST_CASE("an unknown top-level key is a warning") {
	sandbox box;
	auto file = box.write("cfg.json", R"({"bogus":1,"log":{"level":"info"}})");
	std::vector<araya::app::config_warning> warnings;
	auto config = araya::app::resolve_app_config({file}, {}, warnings);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].detail.find("bogus") != std::string::npos);
}

TEST_CASE("component scalars stringify") {
	sandbox box;
	auto file =
		box.write("cfg.json", R"({"components":{"shell":{"timeout_ms":30000,"enable_run_in_background":false}}})");
	std::vector<araya::app::config_warning> warnings;
	auto config = araya::app::resolve_app_config({file}, {}, warnings);
	CHECK(config.components.at("shell").at("timeout_ms") == "30000");
	CHECK(config.components.at("shell").at("enable_run_in_background") == "false");
}

TEST_CASE("validate_component reports unknown keys and rejects malformed values") {
	std::vector<araya::app::config_warning> warnings;
	araya::plugin_config good{{"timeout_ms", "1000"}};
	CHECK_NOTHROW(araya::app::validate_component(stub_lookup, "shell", good, warnings));
	CHECK(warnings.empty());

	araya::plugin_config unknown{{"nope", "1"}};
	araya::app::validate_component(stub_lookup, "shell", unknown, warnings);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].detail.find("nope") != std::string::npos);

	araya::plugin_config malformed{{"timeout_ms", "fast"}};
	CHECK_THROWS_AS(araya::app::validate_component(stub_lookup, "shell", malformed, warnings), araya::config_error);

	warnings.clear();
	araya::app::validate_component(stub_lookup, "ghost", {}, warnings);
	REQUIRE(warnings.size() == 1);
	CHECK(warnings[0].detail.find("unknown component") != std::string::npos);
}

TEST_CASE("render_app_config shows values, provenance, and accepted keys") {
	sandbox box;
	auto file = box.write("cfg.json", R"({"components":{"shell":{"timeout_ms":"1000"}}})");
	std::vector<araya::app::config_warning> warnings;
	auto config = araya::app::resolve_app_config({file}, {}, warnings);
	auto text = araya::app::render_app_config(config, stub_lookup);
	CHECK(text.find("shell") != std::string::npos);
	CHECK(text.find("timeout_ms = 1000") != std::string::npos);
	CHECK(text.find("timeout_ms<integer>") != std::string::npos);
}
