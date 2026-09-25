set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.83 REQUIRED COMPONENTS json)

araya_add_plugin(tools
    SOURCES src/tools.cpp src/plugin.cpp
    DEPS araya::system-prompt Boost::json
    TEST tests/tools_test.cpp
)
