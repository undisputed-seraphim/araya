#include <catch2/catch_test_macros.hpp>

#include "araya/config.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace {

inline constexpr araya::config_key<int> int_key{"int"};
inline constexpr araya::config_key<long long> ll_key{"ll"};
inline constexpr araya::config_key<double> double_key{"double"};
inline constexpr araya::config_key<bool> bool_key{"bool"};
inline constexpr araya::config_key<std::string> str_key{"str"};
inline constexpr araya::config_key<std::string_view> view_key{"view"};
inline constexpr araya::config_key<std::uint8_t> u8_key{"u8"};

enum class level : int { low = 1, high = 2 };
inline constexpr araya::config_key<level> level_key{"level"};

// A plugin-specific word-valued type exercising the customization point.
struct word {
    std::string text;
};

inline constexpr araya::config_key<word> word_key{"word"};

}  // namespace

namespace araya {
template <>
struct config_parser<word> {
    static std::optional<word> parse(std::string_view text) {
        if (text == "hello")
            return word{"hello"};
        return std::nullopt;
    }
};
}  // namespace araya

namespace {

araya::plugin_config cfg(std::initializer_list<araya::plugin_config::value_type> v) {
    return araya::plugin_config(v);
}

}  // namespace

TEST_CASE("arithmetic config values parse through from_chars") {
    auto c = cfg({{"int", "12"}, {"ll", "-42"}, {"double", "3.25"},
                  {"u8", "200"}, {"bool", "true"}});
    araya::plugin_config_view view(c);

    CHECK(view[int_key] == 12);
    CHECK(view[ll_key] == -42);
    CHECK(view[double_key] == 3.25);
    CHECK(view[u8_key] == 200);
    CHECK(view[bool_key] == true);
}

TEST_CASE("bool accepts the four documented forms") {
    for (auto const& [text, want] :
         std::initializer_list<std::pair<std::string, bool>>{
             {"true", true}, {"false", false}, {"1", true}, {"0", false}}) {
        auto c = cfg({{"bool", text}});
        CHECK(araya::plugin_config_view(c)[bool_key] == want);
    }
}

TEST_CASE("string config values copy and string_view borrows the map") {
    auto c = cfg({{"str", "s-value"}, {"view", "v-value"}});
    araya::plugin_config_view view(c);

    CHECK(view[str_key] == "s-value");
    auto v = view[view_key];
    CHECK(v == "v-value");
    // The view borrows the map's storage: same underlying bytes.
    CHECK(v.data() == c.at("view").data());
}

TEST_CASE("enum config values parse through the underlying type") {
    auto c = cfg({{"level", "2"}});
    CHECK(araya::plugin_config_view(c)[level_key] == level::high);
}

TEST_CASE("plugin-specific parser specializations are honored") {
    auto good = cfg({{"word", "hello"}});
    CHECK(araya::plugin_config_view(good)[word_key].text == "hello");

    auto bad = cfg({{"word", "bye"}});
    CHECK_THROWS_AS(araya::plugin_config_view(bad)[word_key],
                    araya::config_error);
}

TEST_CASE("missing keys throw from get and yield nullopt from try_get") {
    auto c = cfg({});
    araya::plugin_config_view view(c);

    CHECK(!view.contains("int"));
    CHECK_THROWS_AS(view[int_key], araya::config_error);
    CHECK(view.try_get(int_key) == std::nullopt);

    auto present = cfg({{"int", "7"}});
    araya::plugin_config_view pview(present);
    CHECK(pview.contains("int"));
    auto got = pview.try_get(int_key);
    REQUIRE(got.has_value());
    CHECK(*got == 7);
}

TEST_CASE("malformed values throw even through try_get (strict values)") {
    for (auto const& text : {"12x", "", "x", "-", "1.5.5"}) {
        auto c = cfg({{"int", text}});
        araya::plugin_config_view view(c);
        CHECK_THROWS_AS(view[int_key], araya::config_error);
        CHECK_THROWS_AS(view.try_get(int_key), araya::config_error);
    }
}

TEST_CASE("overflow and sign mismatches are rejected") {
    auto c = cfg({{"int", "99999999999999999999"}, {"u8", "-3"}});
    araya::plugin_config_view view(c);

    CHECK_THROWS_AS(view[int_key], araya::config_error);
    CHECK_THROWS_AS(view[u8_key], araya::config_error);
}

TEST_CASE("config_error carries key, value, and reason") {
    auto c = cfg({{"int", "12x"}});
    try {
        (void)araya::plugin_config_view(c)[int_key];
        FAIL("expected config_error");
    } catch (araya::config_error const& e) {
        CHECK(e.key() == "int");
        CHECK(e.value() == "12x");
        CHECK(e.reason() == "malformed");
    }
}

TEST_CASE("config keys are constexpr") {
    static_assert(araya::config_key<int>{"children"}.name == "children");
    static_assert(std::same_as<araya::config_key<int>::value_type, int>);
}

TEST_CASE("parse_value serves non-plugin_config inputs") {
    CHECK(araya::parse_value<int>("42") == std::optional<int>(42));
    CHECK(araya::parse_value<long long>("-7") ==
          std::optional<long long>(-7));
    CHECK(araya::parse_value<double>("0.5") == std::optional<double>(0.5));
    CHECK(araya::parse_value<int>("nope") == std::nullopt);
}
