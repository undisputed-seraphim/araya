#include <catch2/catch_test_macros.hpp>

#include "medulla/version.hpp"

TEST_CASE("version is exported") {
    CHECK(medulla::version_major == 0);
    CHECK(medulla::version_minor == 1);
    CHECK(medulla::version_patch == 0);
    CHECK(std::string(medulla::version_string()) == "0.1.0");
}
