# The built-in file and search tools (read/write/edit/glob/grep). The fs
# work is native (std::filesystem); coreutil registers guidance sections
# with the system-prompt registry and tools with the tools registry, and
# resolves relative paths against the calling session's cwd.
set(Boost_NO_BOOST_CMAKE ON)
find_package(Boost 1.91 REQUIRED COMPONENTS json)

araya_add_plugin(coreutil
    SOURCES src/coreutil.cpp
    DEPS araya::session araya::system-prompt araya::tools Boost::json
    TEST tests/coreutil_test.cpp
    TEST_DEPS araya::session araya::system-prompt araya::tools
)
