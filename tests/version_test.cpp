#include <catch2/catch_test_macros.hpp>

#include "araya/version.hpp"

TEST_CASE("version is exported") {
	CHECK(araya::version_major == 0);
	CHECK(araya::version_minor == 1);
	CHECK(araya::version_patch == 0);
	CHECK(std::string(araya::version_string()) == "0.1.0");
}
