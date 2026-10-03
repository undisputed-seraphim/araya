#include <catch2/catch_test_macros.hpp>

#include "araya/logger/logger.hpp"
#include "log_setup.hpp"

#include <cstdlib>
#include <optional>
#include <string>

namespace {

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

} // namespace

TEST_CASE("resolve_log_settings returns the defaults") {
	env_guard file{"ARAYA_LOG_FILE"};
	env_guard level{"ARAYA_LOG_LEVEL"};
	::unsetenv("ARAYA_LOG_FILE");
	::unsetenv("ARAYA_LOG_LEVEL");

	auto settings = araya::app::resolve_log_settings();
	CHECK(settings.file == "araya-tui.log");
	CHECK(settings.level == "info");
	CHECK_FALSE(settings.console);
}

TEST_CASE("the environment overrides only what it sets") {
	env_guard file{"ARAYA_LOG_FILE"};
	env_guard level{"ARAYA_LOG_LEVEL"};
	::setenv("ARAYA_LOG_FILE", "/tmp/araya.log", 1);
	::setenv("ARAYA_LOG_LEVEL", "debug", 1);

	auto settings = araya::app::resolve_log_settings(
		araya::app::log_settings{.file = "base.log", .level = "warn", .console = true});
	CHECK(settings.file == "/tmp/araya.log");
	CHECK(settings.level == "debug");
	CHECK(settings.console == true);
}

TEST_CASE("an empty environment value does not override") {
	env_guard file{"ARAYA_LOG_FILE"};
	::setenv("ARAYA_LOG_FILE", "", 1);

	auto settings = araya::app::resolve_log_settings(araya::app::log_settings{.file = "kept.log"});
	CHECK(settings.file == "kept.log");
}

TEST_CASE("parse_level accepts the four words and rejects the rest") {
	CHECK(araya::logger::parse_level("error") == araya::logger::log_level::error);
	CHECK(araya::logger::parse_level("warn") == araya::logger::log_level::warn);
	CHECK(araya::logger::parse_level("info") == araya::logger::log_level::info);
	CHECK(araya::logger::parse_level("debug") == araya::logger::log_level::debug);
	CHECK_FALSE(araya::logger::parse_level("verbose").has_value());
	CHECK_FALSE(araya::logger::parse_level("").has_value());
}
