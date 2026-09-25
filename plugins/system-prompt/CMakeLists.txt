# Boost.JSON is a compiled component (separate compilation), so the
# plugin and its tests link it.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.91 REQUIRED COMPONENTS json)

araya_add_plugin(system-prompt
    SOURCES src/system_prompt.cpp src/plugin.cpp
    DEPS araya::araya Boost::json
    TEST tests/system_prompt_test.cpp
)
